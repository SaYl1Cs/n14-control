// Test-only device/status scaffolding. Production processor code is included
// from hash-verified upstream files by generate-standard-golden.py.
// No crates, IO devices, system bus, timers or service calls.
#![allow(dead_code, unused_imports)]
use std::{cell::RefCell, collections::{HashMap, VecDeque}, rc::Rc};
macro_rules! error { ($($x:tt)*) => { let _ = format_args!($($x)*); }; }
macro_rules! trace { ($($x:tt)*) => { let _ = format_args!($($x)*); }; }

mod device {
    pub type Duty = u8;
    pub type Temp = f64;
    #[derive(Default)]
    pub struct Device { pub status_history: std::collections::VecDeque<Status> }
    pub struct Status { pub temps: Vec<Temperature> }
    pub struct Temperature { pub name: String, pub temp: f64 }
}
mod repositories { pub mod repository {
    pub type DeviceLock = std::rc::Rc<std::cell::RefCell<crate::device::Device>>;
} }
type AllDevices = Rc<HashMap<String, repositories::repository::DeviceLock>>;

mod setting {
    pub type ProfileUID = String;
    pub type FunctionUID = String;
    pub type Offset = i16;
    const DEFAULT_FUNCTION_UID: &str = "0";
    use crate::device::{Duty, Temp};
    #[derive(Debug, Clone)]
    pub struct TempSource { pub device_uid: String, pub temp_name: String }
    include!("oracle_setting.rs");
}
mod engine {
    use std::rc::Rc;
    use std::hash::{Hash, Hasher};
    use std::collections::HashMap;
    use crate::device::{Duty, Temp};
    use crate::setting::{Function, ProfileUID, TempSource};
    use processors::functions::calc_ideal_stack_size;
    include!("oracle_engine.rs");
    pub mod utils { include!("oracle_utils.rs"); }
    pub mod processors {
        pub mod functions {
            include!("oracle_functions.rs");
            pub fn state(pre: &FunctionStandardPreProcessor,
                         duty: &FunctionDutyThresholdPostProcessor,
                         latch: &FunctionSafetyLatchProcessor) -> String {
                let m = pre.channel_settings_metadata.borrow();
                let m = &m["profile"];
                let d = duty.scheduled_settings_metadata.borrow();
                let d = &d["profile"].last_manual_speeds_set;
                let l = latch.scheduled_settings_metadata.borrow();
                let l = &l["profile"];
                format!("{{\"temperature_history\":{:?},\"duty_history\":{:?},\"ideal_stack_size\":{},\"last_temperature\":{},\"last_duty\":{},\"no_duty_count\":{},\"maximum_no_duty_count\":{}}}",
                    m.temp_hist_stack, d, m.ideal_stack_size, m.last_applied_temp,
                    m.last_applied_duty.map_or("null".into(), |v| v.to_string()),
                    l.no_duty_set_counter, l.max_no_duty_set_count)
            }
        }
        pub mod profiles { include!("oracle_profiles.rs"); }
    }

    // Whitespace input supplied only by the local fixture generator.
    pub fn run(input: &str) {
        use crate::{device::{Device, Status, Temperature}, AllDevices};
        use crate::setting::FunctionKind;
        use std::{cell::RefCell, collections::HashMap};
        use processors::{functions::*, profiles::GraphProcessor};
        let mut tokens = input.split_whitespace();
        fn number<T: std::str::FromStr>(tokens: &mut std::str::SplitWhitespace<'_>) -> T {
            tokens.next().expect("token").parse().ok().expect("number")
        }
        let cases: usize = number(&mut tokens);
        for _ in 0..cases {
            let point_count: usize = number(&mut tokens);
            let mut points = Vec::new();
            for _ in 0..point_count { points.push((number(&mut tokens), number(&mut tokens))); }
            let critical = number(&mut tokens);
            let normalized = utils::normalize_profile(&points, critical, 100);
            let device = Rc::new(RefCell::new(Device::default()));
            let add_temp = |value| device.borrow_mut().status_history.push_back(Status {
                temps: vec![Temperature { name: "temp".into(), temp: value }]
            });
            let initial_count: usize = number(&mut tokens);
            for _ in 0..initial_count { add_temp(number(&mut tokens)); }
            let devices: AllDevices = Rc::new(HashMap::from([("device".into(), Rc::clone(&device))]));
            let pre = FunctionStandardPreProcessor::new(devices);
            let post = pre.create_post_processor();
            let duty = FunctionDutyThresholdPostProcessor::new();
            let latch = FunctionSafetyLatchProcessor::new();
            let graph = GraphProcessor::new();
            let uid = "profile".to_string();
            pre.init_state(&uid); duty.init_state(&uid); latch.init_state(&uid);
            let tick_count: usize = number(&mut tokens);
            for _ in 0..tick_count {
                let function = Function {
                    step_size_min: number(&mut tokens), step_size_max: number(&mut tokens),
                    step_size_min_decreasing: number(&mut tokens), step_size_max_decreasing: number(&mut tokens),
                    kind: FunctionKind::Standard {
                        response_delay: Some(number(&mut tokens)), deviance: Some(number(&mut tokens)),
                        only_downward: Some(number::<u8>(&mut tokens) != 0),
                    },
                    threshold_hopping: number::<u8>(&mut tokens) != 0,
                    bypass_min_at_extremes: number::<u8>(&mut tokens) != 0,
                    ..Default::default()
                };
                let poll_rate = number(&mut tokens);
                let ideal_stack_size = calc_ideal_stack_size(&function, poll_rate);
                let profile = Rc::new(NormalizedGraphProfile {
                    profile_uid: uid.clone(), profile_name: "oracle".into(),
                    speed_profile: normalized.clone(), temp_source: TempSource {
                        device_uid: "device".into(), temp_name: "temp".into()
                    }, function, poll_rate, ideal_stack_size,
                });
                add_temp(number(&mut tokens));
                let mut data = SpeedProfileData { temp: None, duty: None, profile,
                    processing_started: false, safety_latch_triggered: false };
                data.apply(&latch).apply(&pre).apply(&graph).apply(&duty).apply(&post).apply(&latch);
                println!("{{\"duty\":{},\"temperature\":{},\"latch\":{},\"state\":{},\"normalized\":{:?}}}",
                    data.duty.map_or("null".into(), |v| v.to_string()),
                    data.temp.map_or("null".into(), |v| v.to_string()), data.safety_latch_triggered,
                    state(&pre, &duty, &latch), normalized.iter().map(|&(t,d)| vec![t, f64::from(d)]).collect::<Vec<_>>());
            }
        }
        assert!(tokens.next().is_none(), "unused input");
    }
}
fn main() {
    use std::io::Read;
    let mut input = String::new();
    std::io::stdin().read_to_string(&mut input).unwrap();
    engine::run(&input);
}