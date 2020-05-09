use {
    super::generic::{CurrentStateCache, Listener, Message},
    fidl_fuchsia_wlan_policy as fidl_policy,
    futures::{channel::mpsc, future::BoxFuture, prelude::*},
};

#[derive(Copy, Clone, Debug, PartialEq)]
pub struct ConnectedClientInformation {
    count: u8,
}

impl Into<fidl_policy::ConnectedClientInformation> for ConnectedClientInformation {
    fn into(self) -> fidl_policy::ConnectedClientInformation {
        fidl_policy::ConnectedClientInformation { count: Some(self.count) }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ApStatesUpdate {
    pub access_points: Vec<ApStateUpdate>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ApStateUpdate {
    pub state: fidl_policy::OperatingState,
    pub mode: Option<fidl_policy::ConnectivityMode>,
    pub band: Option<fidl_policy::OperatingBand>,
    pub frequency: Option<u32>,
    pub clients: Option<ConnectedClientInformation>,
}

impl Into<Vec<fidl_policy::AccessPointState>> for ApStatesUpdate {
    fn into(self) -> Vec<fidl_policy::AccessPointState> {
        self.access_points
            .iter()
            .map(|ap| fidl_policy::AccessPointState {
                state: Some(ap.state),
                mode: ap.mode,
                band: ap.band,
                frequency: ap.frequency,
                clients: ap.clients.map(|c| c.into()),
            })
            .collect()
    }
}

impl CurrentStateCache for ApStatesUpdate {
    fn default() -> ApStatesUpdate {
        ApStatesUpdate { access_points: vec![] }
    }

    fn merge_in_update(&mut self, update: Self) {
        self.access_points = update.access_points;
    }
}

impl Listener<Vec<fidl_policy::AccessPointState>> for fidl_policy::AccessPointStateUpdatesProxy {
    fn notify_listener(
        self,
        update: Vec<fidl_policy::AccessPointState>,
    ) -> BoxFuture<'static, Option<Box<Self>>> {
        let fut = async move {
            let mut iter = update.into_iter();
            let fut = self.on_access_point_state_update(&mut iter);
            fut.await.ok().map(|()| Box::new(self))
        };
        fut.boxed()
    }
}

// Helpful aliases for servicing client updates
pub type ApMessage = Message<fidl_policy::AccessPointStateUpdatesProxy, ApStatesUpdate>;
pub type ApMessageSender = mpsc::UnboundedSender<ApMessage>;

#[cfg(test)]
mod tests {
    use {
        super::{super::generic::CurrentStateCache, *},
        fidl_fuchsia_wlan_policy as fidl_policy,
    };

    #[test]
    fn merge_updates() {
        let mut current_state_cache = ApStatesUpdate::default();
        assert_eq!(current_state_cache, ApStatesUpdate { access_points: vec![] });

        // Merge in an update with one connected network.
        let update = ApStatesUpdate {
            access_points: vec![{
                ApStateUpdate {
                    state: fidl_policy::OperatingState::Starting,
                    mode: Some(fidl_policy::ConnectivityMode::Unrestricted),
                    band: Some(fidl_policy::OperatingBand::Any),
                    frequency: None,
                    clients: Some(ConnectedClientInformation { count: 0 }),
                }
            }],
        };
        current_state_cache.merge_in_update(update);

        assert_eq!(
            current_state_cache,
            ApStatesUpdate {
                access_points: vec![{
                    ApStateUpdate {
                        state: fidl_policy::OperatingState::Starting,
                        mode: Some(fidl_policy::ConnectivityMode::Unrestricted),
                        band: Some(fidl_policy::OperatingBand::Any),
                        frequency: None,
                        clients: Some(ConnectedClientInformation { count: 0 }),
                    }
                }],
            }
        );
    }

    #[test]
    fn into_fidl() {
        let state = ApStatesUpdate {
            access_points: vec![{
                ApStateUpdate {
                    state: fidl_policy::OperatingState::Starting,
                    mode: Some(fidl_policy::ConnectivityMode::Unrestricted),
                    band: Some(fidl_policy::OperatingBand::Any),
                    frequency: Some(200),
                    clients: Some(ConnectedClientInformation { count: 1 }),
                }
            }],
        };
        let fidl_state: Vec<fidl_policy::AccessPointState> = state.into();
        assert_eq!(
            fidl_state,
            vec![fidl_policy::AccessPointState {
                state: Some(fidl_policy::OperatingState::Starting),
                mode: Some(fidl_policy::ConnectivityMode::Unrestricted),
                band: Some(fidl_policy::OperatingBand::Any),
                frequency: Some(200),
                clients: Some(fidl_policy::ConnectedClientInformation { count: Some(1) }),
            }]
        );
    }
}