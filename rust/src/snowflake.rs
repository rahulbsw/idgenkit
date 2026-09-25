//! Snowflake: time-ordered 64-bit IDs.
//!
//! Layout (compatible with <https://github.com/dustinrouillard/snowflake-id>):
//! `| 42 bits: ms since epoch | 10 bits: machine id | 12 bits: sequence |`

use crate::{now_ms, Error};
use std::sync::atomic::{AtomicU64, Ordering};

pub const TIMESTAMP_BITS: u32 = 42;
pub const MACHINE_ID_BITS: u32 = 10;
pub const SEQUENCE_BITS: u32 = 12;
pub const MAX_MACHINE_ID: u16 = (1 << MACHINE_ID_BITS) - 1;
pub const MAX_SEQUENCE: u16 = (1 << SEQUENCE_BITS) - 1;
pub const MAX_TIMESTAMP_DELTA: u64 = (1 << TIMESTAMP_BITS) - 1;

const MACHINE_SHIFT: u32 = SEQUENCE_BITS;
const TIMESTAMP_SHIFT: u32 = SEQUENCE_BITS + MACHINE_ID_BITS;

/// A decoded Snowflake ID.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Parts {
    pub timestamp_ms: u64,
    pub machine_id: u16,
    pub sequence: u16,
}

pub fn compose(timestamp_ms: u64, machine_id: u16, sequence: u16, epoch_ms: u64) -> Result<u64, Error> {
    if machine_id > MAX_MACHINE_ID {
        return Err(Error::MachineId);
    }
    if sequence > MAX_SEQUENCE {
        return Err(Error::Sequence);
    }
    let delta = timestamp_ms.checked_sub(epoch_ms).filter(|d| *d <= MAX_TIMESTAMP_DELTA).ok_or(Error::TimeRange)?;
    Ok((delta << TIMESTAMP_SHIFT) | ((machine_id as u64) << MACHINE_SHIFT) | sequence as u64)
}

pub fn parse(id: u64, epoch_ms: u64) -> Parts {
    Parts {
        timestamp_ms: (id >> TIMESTAMP_SHIFT) + epoch_ms,
        machine_id: ((id >> MACHINE_SHIFT) & MAX_MACHINE_ID as u64) as u16,
        sequence: (id & MAX_SEQUENCE as u64) as u16,
    }
}

/// Lock-free Snowflake generator, safe to share across threads.
///
/// After 4096 IDs in one millisecond it waits for the next millisecond. If the
/// wall clock moves backwards it keeps issuing from the last observed
/// millisecond, so IDs never decrease.
pub struct Snowflake {
    machine_id: u64,
    epoch_ms: u64,
    /// `last_ms << SEQUENCE_BITS | sequence`, updated with a single CAS.
    state: AtomicU64,
}

impl Snowflake {
    /// `epoch_ms` is a custom epoch in ms since the Unix epoch (0 = Unix epoch).
    pub fn new(machine_id: u16, epoch_ms: u64) -> Result<Self, Error> {
        if machine_id > MAX_MACHINE_ID {
            return Err(Error::MachineId);
        }
        Ok(Snowflake { machine_id: machine_id as u64, epoch_ms, state: AtomicU64::new(0) })
    }

    pub fn next_id(&self) -> Result<u64, Error> {
        self.next_id_with(now_ms)
    }

    pub(crate) fn next_id_with(&self, mut clock: impl FnMut() -> u64) -> Result<u64, Error> {
        let seq_mask = MAX_SEQUENCE as u64;
        loop {
            let old = self.state.load(Ordering::Relaxed);
            let (last_ms, seq) = (old >> SEQUENCE_BITS, old & seq_mask);
            let now = clock();
            let (ms, next) = if now > last_ms {
                (now, 0)
            } else if seq < seq_mask {
                (last_ms, seq + 1)
            } else {
                while clock() <= last_ms {
                    std::hint::spin_loop();
                }
                continue;
            };
            let delta = ms.checked_sub(self.epoch_ms).filter(|d| *d <= MAX_TIMESTAMP_DELTA).ok_or(Error::TimeRange)?;
            if self
                .state
                .compare_exchange(old, (ms << SEQUENCE_BITS) | next, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
            {
                return Ok((delta << TIMESTAMP_SHIFT) | (self.machine_id << MACHINE_SHIFT) | next);
            }
        }
    }

    pub fn parse(&self, id: u64) -> Parts {
        parse(id, self.epoch_ms)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_vectors;

    fn scripted_clock(readings: Vec<u64>) -> impl FnMut() -> u64 {
        let mut i = 0;
        move || {
            let v = readings[i];
            i = (i + 1).min(readings.len() - 1);
            v
        }
    }

    #[test]
    fn sequence_vectors() {
        let rows = test_vectors("snowflake_sequence.txt");
        assert!(rows.len() > 10);
        let mut g = Snowflake::new(0, 0).unwrap();
        let mut prev = 0;
        for r in &rows {
            match r[0].as_str() {
                "gen" => {
                    g = Snowflake::new(r[1].parse().unwrap(), r[2].parse().unwrap()).unwrap();
                    prev = 0;
                }
                "fill" => {
                    let clock: u64 = r[2].parse().unwrap();
                    for _ in 0..r[1].parse::<u32>().unwrap() {
                        let id = g.next_id_with(|| clock).unwrap();
                        assert!(id > prev, "{r:?}: {id} after {prev}");
                        prev = id;
                    }
                }
                "next" => {
                    let readings = r[1].split(',').map(|s| s.parse().unwrap()).collect();
                    let got = g.next_id_with(scripted_clock(readings));
                    if r[2] == "error" {
                        assert!(got.is_err(), "{r:?}: got {got:?}, want an error");
                    } else {
                        prev = got.unwrap();
                        assert_eq!(prev, r[2].parse::<u64>().unwrap(), "{r:?}");
                    }
                }
                other => panic!("bad line {other}"),
            }
        }
    }

    #[test]
    fn clock_backwards_keeps_last_millisecond() {
        let g = Snowflake::new(1, 0).unwrap();
        let first = g.next_id().unwrap();
        let future = now_ms() + 5;
        g.state.store(future << SEQUENCE_BITS, Ordering::SeqCst);
        let second = g.next_id().unwrap();
        assert!(second > first);
        assert_eq!(g.parse(second), Parts { timestamp_ms: future, machine_id: 1, sequence: 1 });
    }
}
