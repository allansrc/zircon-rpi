// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        commands::{types::*, utils},
        types::Error,
    },
    argh::FromArgs,
    async_trait::async_trait,
    fuchsia_inspect_node_hierarchy::NodeHierarchy,
    selectors,
};

/// Lists all available full selectors (component selector + tree selector).
/// If a selector is provided, it’ll only print selectors for that component.
/// If a full selector (component + tree) is provided, it lists all selectors under the given node.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "selectors")]
pub struct SelectorsCommand {
    #[argh(option)]
    /// the name of the manifest file that we are interested in. If this is provided, the output
    /// will only contain monikers for components whose url contains the provided name.
    pub manifest: Option<String>,

    #[argh(positional)]
    /// selectors for which the selectors should be queried. Minimum: 1 unless `--manifest` is set.
    /// When `--manifest` is provided then the selectors should be tree selectors, otherwise
    /// they can be component selectors or full selectors.
    pub selectors: Vec<String>,
}

#[async_trait]
impl Command for SelectorsCommand {
    type Result = Vec<String>;

    async fn execute(&self) -> Result<Self::Result, Error> {
        if self.selectors.is_empty() && self.manifest.is_none() {
            return Err(Error::invalid_arguments("Expected 1 or more selectors. Got zero."));
        }
        let selectors = utils::get_selectors_for_manifest(&self.manifest, &self.selectors).await?;
        let mut result = utils::fetch_data(&selectors)
            .await?
            .into_iter()
            .flat_map(|(component_selector, hierarchy)| {
                get_selectors(component_selector, hierarchy)
            })
            .collect::<Vec<_>>();
        result.sort();
        Ok(result)
    }
}

fn get_selectors(component_selector: String, hierarchy: NodeHierarchy) -> Vec<String> {
    hierarchy
        .property_iter()
        .flat_map(|(node_path, maybe_property)| maybe_property.map(|prop| (node_path, prop)))
        .map(|(node_path, property)| {
            let node_selector = node_path
                .iter()
                .map(|s| selectors::sanitize_string_for_selectors(s))
                .collect::<Vec<String>>()
                .join("/");
            let property_selector = selectors::sanitize_string_for_selectors(property.name());
            format!("{}:{}:{}", component_selector, node_selector, property_selector)
        })
        .collect()
}
