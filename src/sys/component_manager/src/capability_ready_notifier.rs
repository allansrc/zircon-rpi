// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        error::ModelError,
        events::{filter::EventFilter, synthesizer::EventSynthesisProvider},
        hooks::{
            Event, EventError, EventErrorPayload, EventPayload, EventType, Hook, HooksRegistration,
        },
        model::Model,
        moniker::AbsoluteMoniker,
        realm::Realm,
        rights::{Rights, READ_RIGHTS, WRITE_RIGHTS},
    },
    async_trait::async_trait,
    cm_rust::{CapabilityPath, ExposeDecl, ExposeDirectoryDecl, ExposeProtocolDecl},
    fidl::endpoints::{Proxy, ServerEnd},
    fidl_fuchsia_io::{self as fio, DirectoryProxy, NodeEvent, NodeMarker, NodeProxy},
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::stream::StreamExt,
    io_util,
    log::*,
    std::sync::{Arc, Weak},
};

/// Awaits for `Started` events and for each capability exposed to framework, dispatches a
/// `CapabilityReady` event.
pub struct CapabilityReadyNotifier {
    model: Weak<Model>,
}

impl CapabilityReadyNotifier {
    pub fn new(model: Weak<Model>) -> Self {
        Self { model }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "CapabilityReadyNotifier",
            vec![EventType::Started],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    async fn on_component_started(
        self: &Arc<Self>,
        target_moniker: &AbsoluteMoniker,
        outgoing_dir: &DirectoryProxy,
        expose_decls: Vec<ExposeDecl>,
    ) -> Result<(), ModelError> {
        // Forward along errors into the new task so that dispatch can forward the
        // error as an event.
        let outgoing_node_result = clone_outgoing_root(&outgoing_dir, &target_moniker).await;

        // Don't block the handling on the event on the exposed capabilities being ready
        let this = self.clone();
        let moniker = target_moniker.clone();
        fasync::spawn(async move {
            // If we can't find the realm then we can't dispatch any CapabilityReady event,
            // error or otherwise. This isn't necessarily an error as the model or realm might've been
            // destroyed in the intervening time, so we just exit early.
            let target_realm = match this.model.upgrade() {
                Some(model) => {
                    if let Ok(realm) = model.look_up_realm(&moniker).await {
                        realm
                    } else {
                        return;
                    }
                }
                None => return,
            };

            this.dispatch_capabilities_ready(outgoing_node_result, expose_decls, &target_realm)
                .await;
        });
        Ok(())
    }

    /// Waits for the OnOpen event on the directory. This will hang until the component starts
    /// serving that directory. The directory should have been cloned/opened with DESCRIBE.
    async fn wait_for_on_open(
        &self,
        node: &NodeProxy,
        target_moniker: &AbsoluteMoniker,
        path: String,
    ) -> Result<(), ModelError> {
        let mut events = node.take_event_stream();
        match events.next().await {
            Some(Ok(NodeEvent::OnOpen_ { s: status, info: _ })) => zx::Status::ok(status)
                .map_err(|_| ModelError::open_directory_error(target_moniker.clone(), path)),
            _ => Err(ModelError::open_directory_error(target_moniker.clone(), path)),
        }
    }

    /// Waits for the outgoing directory to be ready and then notifies hooks of all the capabilities
    /// inside it that were exposed to the framework by the component.
    async fn dispatch_capabilities_ready(
        &self,
        outgoing_node_result: Result<NodeProxy, ModelError>,
        expose_decls: Vec<ExposeDecl>,
        target_realm: &Arc<Realm>,
    ) {
        let capability_ready_events =
            self.create_events(outgoing_node_result, expose_decls, target_realm).await;
        for capability_ready_event in capability_ready_events {
            target_realm.hooks.dispatch(&capability_ready_event).await.unwrap_or_else(|e| {
                error!("Error notifying capability ready for {}: {:?}", target_realm.abs_moniker, e)
            });
        }
    }

    async fn create_events(
        &self,
        outgoing_node_result: Result<NodeProxy, ModelError>,
        expose_decls: Vec<ExposeDecl>,
        target_realm: &Arc<Realm>,
    ) -> Vec<Event> {
        // Forward along the result for opening the outgoing directory into the CapabilityReady
        // dispatch in order to propagate any potential errors as an event.
        let outgoing_dir_result = async move {
            let outgoing_node = outgoing_node_result?;
            self.wait_for_on_open(&outgoing_node, &target_realm.abs_moniker, "/".to_string())
                .await?;
            io_util::node_to_directory(outgoing_node).map_err(|_| {
                ModelError::open_directory_error(target_realm.abs_moniker.clone(), "/")
            })
        }
        .await;

        let mut events = Vec::new();
        for expose_decl in expose_decls {
            let event = match expose_decl {
                ExposeDecl::Directory(ExposeDirectoryDecl {
                    source_path,
                    target_path,
                    rights,
                    ..
                }) => {
                    self.create_event(
                        &target_realm,
                        outgoing_dir_result.as_ref(),
                        fio::MODE_TYPE_DIRECTORY,
                        Rights::from(rights.unwrap_or(*READ_RIGHTS)),
                        source_path,
                        target_path,
                    )
                    .await
                }
                ExposeDecl::Protocol(ExposeProtocolDecl { source_path, target_path, .. }) => {
                    self.create_event(
                        &target_realm,
                        outgoing_dir_result.as_ref(),
                        fio::MODE_TYPE_SERVICE,
                        Rights::from(*WRITE_RIGHTS),
                        source_path,
                        target_path,
                    )
                    .await
                }
                _ => continue,
            };
            events.push(event);
        }

        events
    }

    /// Creates an event with the directory at the given `target_path` inside the provided
    /// outgoing directory if the capability is available.
    async fn create_event(
        &self,
        target_realm: &Arc<Realm>,
        outgoing_dir_result: Result<&DirectoryProxy, &ModelError>,
        mode: u32,
        rights: Rights,
        source_path: CapabilityPath,
        target_path: CapabilityPath,
    ) -> Event {
        let target_path = target_path.to_string();

        let node_result = async move {
            // DirProxy.open fails on absolute paths.
            let source_path = source_path.to_string();
            let canonicalized_path = io_util::canonicalize_path(&source_path);
            let outgoing_dir = outgoing_dir_result.map_err(|e| e.clone())?;

            let (node, server_end) = fidl::endpoints::create_proxy::<NodeMarker>().unwrap();

            outgoing_dir
                .open(
                    rights.into_legacy() | fio::OPEN_FLAG_DESCRIBE,
                    mode,
                    &canonicalized_path,
                    ServerEnd::new(server_end.into_channel()),
                )
                .map_err(|_| {
                    ModelError::open_directory_error(
                        target_realm.abs_moniker.clone(),
                        source_path.clone(),
                    )
                })?;
            self.wait_for_on_open(&node, &target_realm.abs_moniker, canonicalized_path.to_string())
                .await?;
            Ok(node)
        }
        .await;

        match node_result {
            Ok(node) => Event::new(
                &target_realm,
                Ok(EventPayload::CapabilityReady { path: target_path, node }),
            ),
            Err(e) => Event::new(
                &target_realm,
                Err(EventError::new(&e, EventErrorPayload::CapabilityReady { path: target_path })),
            ),
        }
    }
}

async fn clone_outgoing_root(
    outgoing_dir: &DirectoryProxy,
    target_moniker: &AbsoluteMoniker,
) -> Result<NodeProxy, ModelError> {
    let outgoing_dir = io_util::clone_directory(
        &outgoing_dir,
        fio::CLONE_FLAG_SAME_RIGHTS | fio::OPEN_FLAG_DESCRIBE,
    )
    .map_err(|_| ModelError::open_directory_error(target_moniker.clone(), "/"))?;
    let outgoing_dir_channel = outgoing_dir
        .into_channel()
        .map_err(|_| ModelError::open_directory_error(target_moniker.clone(), "/"))?;
    Ok(NodeProxy::from_channel(outgoing_dir_channel))
}

#[async_trait]
impl EventSynthesisProvider for CapabilityReadyNotifier {
    async fn provide(&self, realm: Arc<Realm>, filter: EventFilter) -> Vec<Event> {
        let maybe_outgoing_node_result =
            async {
                let execution = realm.lock_execution().await;
                if execution.runtime.is_none() {
                    return None;
                }
                let runtime = execution.runtime.as_ref().unwrap();
                let out_dir = match runtime.outgoing_dir.as_ref().ok_or(
                    ModelError::open_directory_error(realm.abs_moniker.clone(), "/".to_string()),
                ) {
                    Ok(out_dir) => out_dir,
                    Err(e) => return Some(Err(e)),
                };
                Some(clone_outgoing_root(&out_dir, &realm.abs_moniker).await)
            }
            .await;

        let outgoing_node_result = match maybe_outgoing_node_result {
            None => return vec![],
            Some(result) => result,
        };

        let expose_decls = {
            if let Some(state) = realm.lock_state().await.as_ref() {
                let expose_decls = state.decl().get_self_capabilities_exposed_to_framework();
                if expose_decls.is_empty() {
                    return Vec::new();
                }
                expose_decls
            } else {
                return Vec::new();
            }
        };

        let expose_decls = expose_decls
            .into_iter()
            .filter(|expose_decl| match expose_decl {
                ExposeDecl::Directory(ExposeDirectoryDecl { target_path, .. })
                | ExposeDecl::Protocol(ExposeProtocolDecl { target_path, .. }) => {
                    filter.contains("path", vec![target_path.to_string()])
                }
                _ => false,
            })
            .collect();

        self.create_events(outgoing_node_result, expose_decls, &realm).await
    }
}

#[async_trait]
impl Hook for CapabilityReadyNotifier {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        match &event.result {
            Ok(EventPayload::Started { runtime, component_decl, .. }) => {
                let expose_decls = component_decl.get_self_capabilities_exposed_to_framework();
                if expose_decls.is_empty() {
                    return Ok(());
                }
                if let Some(outgoing_dir) = &runtime.outgoing_dir {
                    self.on_component_started(&event.target_moniker, outgoing_dir, expose_decls)
                        .await?;
                }
            }
            _ => {}
        }
        Ok(())
    }
}
