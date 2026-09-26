//! Minimal std-only benchmark harness (no criterion): warm up, then take the
//! best of several timed batches.

use std::hint::black_box;
use std::sync::Arc;
use std::thread;
use std::time::Instant;
use idgenkit::nanoid::{self, CustomAlphabet};
use idgenkit::relid::RelativeId;
use idgenkit::snowflake::Snowflake;
use idgenkit::ulid::{MonotonicGenerator, Ulid};
use idgenkit::uuid::{MonotonicV7Generator, Uuid};

fn bench<T, F: FnMut() -> T>(name: &str, iters: u64, mut f: F) {
    for _ in 0..iters / 10 {
        black_box(f());
    }
    let mut best = f64::MAX;
    for _ in 0..5 {
        let start = Instant::now();
        for _ in 0..iters {
            black_box(f());
        }
        best = best.min(start.elapsed().as_nanos() as f64 / iters as f64);
    }
    println!("rust    {name:<28} {best:10.1} ns/op {:>14.0} ops/s", 1e9 / best);
}

fn bench_parallel<F: Fn() + Send + Sync + 'static>(name: &str, iters: u64, threads: usize, f: F) {
    let f = Arc::new(f);
    let start = Instant::now();
    let handles: Vec<_> = (0..threads)
        .map(|_| {
            let f = f.clone();
            thread::spawn(move || {
                for _ in 0..iters {
                    f();
                }
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
    let total = iters * threads as u64;
    let ns = start.elapsed().as_nanos() as f64 / total as f64;
    println!("rust    {:<28} {ns:10.1} ns/op {:>14.0} ops/s", format!("{name} x{threads}"), 1e9 / ns);
}

fn main() {
    let n: u64 = std::env::var("BENCH_N").ok().and_then(|v| v.parse().ok()).unwrap_or(1_000_000);
    let threads = thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(8);
    println!("# Rust, N={n}");

    let mono = MonotonicGenerator::new();
    let uuid_mono = MonotonicV7Generator::new();
    let sf = Arc::new(Snowflake::new(1, 0).unwrap());
    let hex = CustomAlphabet::new("0123456789abcdef", 21).unwrap();
    let sample = Ulid::new().to_string();

    bench("ulid.new", n, Ulid::new);
    bench("ulid.new.to_string", n, || Ulid::new().to_string());
    bench("ulid.monotonic", n, || mono.next().unwrap());
    bench("ulid.parse", n, || Ulid::parse(black_box(&sample)).unwrap());
    bench("uuid.v4", n, Uuid::new_v4);
    bench("uuid.v7", n, || Uuid::new_v7().unwrap());
    bench("uuid.v7.monotonic", n, || uuid_mono.next().unwrap());
    let relid = RelativeId::new(b"bench-only-secret-0123456789", "orders").unwrap();
    bench("relid.generate", n, || relid.generate(black_box("customer-42")).unwrap());
    bench("relid.monotonic", n, || relid.monotonic(black_box("customer-42")).unwrap());
    bench("snowflake.next_id", n, || sf.next_id().unwrap());
    bench("nanoid(21)", n, nanoid::nanoid);
    bench("nanoid.custom(hex,21)", n, || hex.generate());

    let sf2 = sf.clone();
    bench_parallel("snowflake.next_id", n / threads as u64, threads, move || {
        black_box(sf2.next_id().unwrap());
    });
    bench_parallel("ulid.new", n / threads as u64, threads, || {
        black_box(Ulid::new());
    });
}
