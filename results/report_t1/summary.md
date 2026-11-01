# TailTrace results

| run | arrivals | pps | dispersion (1 ms) | max pkts in 100 us | lost | kernel drops | e2e p99.9 (us) | from_schedule p50 / p99 / p99.9 / max (us) |
|---|---|---|---|---|---|---|---|---|
| poisson_block_t1 | poisson | 49952 | 0.99 | 16 | 0 | 0 | 1523.7 | 15.4 / 696.3 / 4423.7 / 8800.2 |
| hawkes_block_t1 | hawkes | 50088 | 9.52 | 45 | 0 | 0 | 7634.9 | 14.7 / 966.7 / 7733.2 / 13177.0 |
| poisson_busy_t1 | poisson | 49952 | 0.99 | 16 | 0 | 0 | 5472.3 | 2.8 / 432.1 / 5472.3 / 14485.8 |
| hawkes_busy_t1 | hawkes | 50088 | 9.52 | 45 | 0 | 0 | 1409.0 | 3.0 / 557.1 / 2588.7 / 5447.7 |

## Stage breakdown (us, p50 / p99 / p99.9)

| run | sender_lateness | send_to_kernel | kernel_to_app | batch_wait | parse |
|---|---|---|---|---|---|
| poisson_block_t1 | 0.1 / 364.5 / 4358.1 | 1.0 / 2.1 / 17.5 | 13.0 / 78.8 / 1515.5 | 0.1 / 2.7 / 14.3 | 0.1 / 0.1 / 0.2 |
| hawkes_block_t1 | 0.7 / 354.3 / 2162.7 | 0.8 / 2.2 / 15.7 | 12.0 / 354.3 / 7634.9 | 0.1 / 3.6 / 12.9 | 0.1 / 0.1 / 0.2 |
| poisson_busy_t1 | 0.1 / 22.4 / 1220.6 | 0.8 / 1.9 / 13.8 | 1.8 / 143.4 / 5472.3 | 0.0 / 1.8 / 8.7 | 0.0 / 0.1 / 0.1 |
| hawkes_busy_t1 | 0.1 / 251.9 / 2490.4 | 0.7 / 2.1 / 17.5 | 1.8 / 38.7 / 1384.4 | 0.0 / 1.2 / 8.6 | 0.0 / 0.1 / 0.1 |

## Tail packets vs the traffic just before them

| run | p99.9_us | mean_preceding_all | mean_preceding_tail | mean_batch_all | mean_batch_tail |
|---|---|---|---|---|---|
| poisson_block_t1 | 7070.0 | 5.0 | 5.61 | 2.17 | 7.9 |
| hawkes_block_t1 | 3410.0 | 7.23 | 5.64 | 2.62 | 30.53 |
| poisson_busy_t1 | 11039.8 | 5.0 | 5.52 | 1.43 | 32.0 |
| hawkes_busy_t1 | 2977.9 | 7.23 | 7.99 | 1.27 | 2.77 |
