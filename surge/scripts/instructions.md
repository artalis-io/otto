# Tuning Campaign Instructions

## Machine

Hetzner Cloud CCX63: 48 vCPU AMD, 192GB RAM, ~€0.85/hr.
Ubuntu 22.04 or 24.04, any region.

## Setup (~5 min)

```bash
apt update && apt install -y build-essential git

git clone <your-repo-url> otto && cd otto/surge
make bench_tune
make bench-download
```

## Launch

```bash
# Priority 8 cells first (~3 days)
nohup ./scripts/tune_matrix.sh --threads 40 --priority-only > campaign.log 2>&1 &

# Or all 20 cells (~10 days)
nohup ./scripts/tune_matrix.sh --threads 40 > campaign.log 2>&1 &
```

## Monitor

```bash
tail -f campaign.log
ls -la benchmarks/results/matrix/*.jsonl
```

## Resume

If the SSH connection drops or you reboot, just re-run the same command.
Checkpoint files track per-evaluation progress — completed configs and tiers are skipped.

## After Tuning

1. Copy `benchmarks/results/matrix/*.jsonl` off the server
2. Extract winning params from each checkpoint file
3. Update `k_profile_matrix` in `src/sg_profile_matrix.c`
4. Re-validate with full instance sets (`--max-instances 0`)
5. Run `make test` to verify backward compatibility
