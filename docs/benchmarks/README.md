# Benchmarks

## Pipeline benchmark (end-to-end)
Build the benchmark target and run:

```powershell
cmake --build build --config Release
.\build\bin\Release\argentum_pipeline_benchmark.exe
```

This measures publish throughput and end-to-end completion for the in-proc bus
and asynchronous writer. It also reports p50/p95/p99/p99.9 end-to-end latency
and drop counts. Record results with hardware + compiler details.
