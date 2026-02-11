# Li & Lim PDPTW Benchmarks

This directory is for Li & Lim PDPTW instance files (`*.txt`).

Included fixture:
- `LC101-mini.txt`: small synthetic smoke-test instance used by `surge/tests/test_surge.c`.

Run benchmark:

```bash
make -C surge bench-li-lim
```

Or point to a different dataset directory:

```bash
./surge/bench_li_lim --dir /path/to/li-lim
```
