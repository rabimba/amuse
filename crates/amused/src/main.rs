//! amused — Amuse agent host daemon (M1 skeleton).
//!
//! Final responsibilities (spec: docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md):
//!   - gateway: JSON-RPC 2.0 over WebSocket (UDS now, TLS+auth for remote later)
//!   - store: SQLite WAL event-sourced session/event log
//!   - scheduler: persistent RRULE/cron jobs, triggers, watchers
//!   - model proxy: injects provider API keys (hatch never holds them)
//!   - hosts Sentinel (policy/approvals/ledger/egress) and authd (vault)
//!
//! M1 scope: config discovery + status reporting only.

use std::path::PathBuf;

fn data_dir() -> PathBuf {
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = std::env::var_os("HOME") {
            return PathBuf::from(home).join("Library/Application Support/Amuse");
        }
    }
    #[cfg(not(target_os = "macos"))]
    {
        if let Some(xdg) = std::env::var_os("XDG_DATA_HOME") {
            return PathBuf::from(xdg).join("amuse");
        }
        if let Some(home) = std::env::var_os("HOME") {
            return PathBuf::from(home).join(".local/share/amuse");
        }
    }
    PathBuf::from(".amuse")
}

fn main() {
    let dir = data_dir();
    println!("amused 0.1.0 (M1 skeleton)");
    println!("data dir: {}", dir.display());
    println!("status: gateway/store/scheduler/sentinel/authd not yet implemented — see spec §3");
}
