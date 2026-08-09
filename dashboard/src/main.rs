// dashboard/src/main.rs — Stage 6, Part E/F: the observability dashboard.
//
// WHAT THIS IS: a completely separate process from the C++ trading engine, with
// NO channel back into it at all — not a socket, not shared memory, nothing. It
// polls a plain JSON file (`telemetry/telemetry_publisher.hpp`'s
// `publish_snapshot`, written atomically via temp-file-then-rename by
// `tools/kalshi_pipeline.cpp`'s existing 500ms loop) on its own schedule and
// serves a browser page that polls THIS process the same way. That's not a
// simplification — it's the point: "the dashboard should be allowed to become
// stale. The trading engine must never wait for the dashboard" (see
// `pipeline/brain.hpp`'s own header comment on why `Brain`'s telemetry hooks are
// nullptr-by-default and O(1)/allocation-free). A dashboard that could ever
// stall `Brain`'s single pinned thread would be a correctness bug, not a UX one
// — the file-polling design makes that structurally impossible, not just
// unlikely: this process can crash, hang, or never start at all, and the C++
// side never notices or cares.
//
// PASSTHROUGH, NOT A SCHEMA COPY: `/api/snapshot` re-serializes whatever valid
// JSON is currently in the file, rather than deserializing into a hand-written
// Rust struct mirroring `telemetry_publisher.hpp`'s field set. A struct-per-field
// mirror is the more obvious design, but it's also a silent drift hazard: add a
// field on the C++ side and forget the Rust side, and the dashboard just quietly
// never shows it — no compile error, no runtime error, nothing. Passthrough
// means the frontend (`static/index.html`) is the only place that needs to know
// the schema, and it fails LOUDLY (a table cell renders `undefined`, immediately
// visible) rather than silently, if the two drift.
use axum::{
    extract::State,
    http::StatusCode,
    response::{Html, IntoResponse},
    routing::get,
    Json, Router,
};
use serde_json::{json, Value};
use std::{net::SocketAddr, path::PathBuf, sync::Arc};

struct AppState {
    telemetry_path: PathBuf,
}

struct Args {
    telemetry_path: PathBuf,
    host: String,
    port: u16,
}

fn print_usage() {
    eprintln!(
        "usage: px-dashboard [--telemetry-path FILE] [--host HOST] [--port PORT]\n\
         \x20 --telemetry-path FILE  JSON snapshot to poll (default: telemetry/snapshot.json,\n\
         \x20                       relative to the current directory -- same default and same\n\
         \x20                       relative-path convention as tools/kalshi_pipeline.cpp's own\n\
         \x20                       --telemetry-out. Run both from the repo root, or pass matching\n\
         \x20                       explicit paths, so they agree on which file they mean.)\n\
         \x20 --host HOST            bind address (default: 127.0.0.1 -- not exposed off this\n\
         \x20                       machine unless you explicitly opt in, e.g. --host 0.0.0.0)\n\
         \x20 --port PORT            bind port (default: 7878)"
    );
}

fn parse_args() -> Option<Args> {
    let mut telemetry_path = PathBuf::from("telemetry/snapshot.json");
    let mut host = "127.0.0.1".to_string();
    let mut port: u16 = 7878;

    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--telemetry-path" => telemetry_path = PathBuf::from(it.next()?),
            "--host" => host = it.next()?,
            "--port" => port = it.next()?.parse().ok()?,
            "-h" | "--help" => {
                print_usage();
                std::process::exit(0);
            }
            other => {
                eprintln!("unknown argument: {other}");
                return None;
            }
        }
    }
    Some(Args { telemetry_path, host, port })
}

// Embedded at COMPILE time, not read from disk at startup -- the dashboard
// binary is fully self-contained (no external CDN scripts/fonts either, see
// static/index.html's own header comment), so `cargo build --release` produces
// one artifact with no runtime asset dependency to go missing.
static INDEX_HTML: &str = include_str!("../static/index.html");

async fn index() -> Html<&'static str> {
    Html(INDEX_HTML)
}

async fn health() -> Json<Value> {
    Json(json!({"status": "ok"}))
}

// Reads + validates the telemetry file fresh on EVERY request -- no caching,
// no background poll loop of its own. The file is small (a handful of KB even
// with the recent-events rings near full) and this endpoint is polled at
// human-dashboard rates (~1/sec from static/index.html's own JS), so "read a
// small file from disk once a second" is nowhere near a real cost worth
// engineering around; simplicity (nothing to get out of sync, nothing to leak,
// nothing racy of its own) wins outright at this scale.
//
// Returns 503, not a stale/synthetic 200, when the file is missing, unreadable,
// or not valid JSON -- e.g. the split second between publish_snapshot's
// temp-file write and its rename (see that function's own comment: rename is
// atomic, so a reader NEVER sees a torn file, but it can legitimately see "no
// file yet" on this process's very first request, or "old file gone, new one
// not yet renamed in" for a few microseconds). The frontend treats 503 the same
// as a network failure -- shows last-known-good data with an explicit stale
// marker, never silently swaps in an error object as if it were real telemetry.
async fn get_snapshot(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    match tokio::fs::read_to_string(&state.telemetry_path).await {
        Ok(text) => match serde_json::from_str::<Value>(&text) {
            Ok(v) => (StatusCode::OK, Json(v)),
            Err(e) => (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(json!({
                    "error": format!("telemetry file did not parse as JSON: {e}"),
                    "path": state.telemetry_path.display().to_string(),
                })),
            ),
        },
        Err(e) => (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(json!({
                "error": format!("cannot read telemetry file: {e}"),
                "path": state.telemetry_path.display().to_string(),
            })),
        ),
    }
}

#[tokio::main]
async fn main() {
    let Some(args) = parse_args() else {
        print_usage();
        std::process::exit(2);
    };

    let state = Arc::new(AppState { telemetry_path: args.telemetry_path.clone() });

    let app = Router::new()
        .route("/", get(index))
        .route("/api/health", get(health))
        .route("/api/snapshot", get(get_snapshot))
        .with_state(state);

    let addr_str = format!("{}:{}", args.host, args.port);
    let addr: SocketAddr = match addr_str.parse() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("invalid --host/--port ('{addr_str}'): {e}");
            std::process::exit(2);
        }
    };

    eprintln!(
        "px-dashboard: polling '{}' -- serving http://{addr}/",
        args.telemetry_path.display()
    );
    let listener = match tokio::net::TcpListener::bind(addr).await {
        Ok(l) => l,
        Err(e) => {
            eprintln!("failed to bind {addr}: {e}");
            std::process::exit(1);
        }
    };
    if let Err(e) = axum::serve(listener, app).await {
        eprintln!("server error: {e}");
        std::process::exit(1);
    }
}
