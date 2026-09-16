Machine: 12th Gen Intel(R) Core(TM) i5-1245U, 12 logical CPUs, 7.6 GiB RAM, kernel 6.6.87.2-microsoft-standard-WSL2, c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0.

Median of 3 runs per case. Throughput = messages processed per second. Latency = frame received -> processing finished (server-side, steady clock).

| Case | Workers | Work/msg | Offered | Throughput (min-max) | Dropped | p50 | p95 | p99 | Server CPU | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| light_w1 | 1 | 0 us | max | 339,969/s (286,992-341,519) | 6.5% | 2.9 ms | 18 ms | 30 ms | 671% | 4.6 MB |
| light_w2 | 2 | 0 us | max | 381,141/s (308,946-415,128) | 0.0% | 2.4 ms | 11 ms | 19 ms | 739% | 4.5 MB |
| light_w4 | 4 | 0 us | max | 311,397/s (282,107-343,802) | 0.0% | 2.6 ms | 13 ms | 20 ms | 804% | 4.4 MB |
| cpu20_w1 | 1 | 20 us | max | 24,887/s (24,024-26,412) | 94.4% | 160 ms | 270 ms | 328 ms | 800% | 4.8 MB |
| cpu20_w2 | 2 | 20 us | max | 47,928/s (39,992-49,226) | 89.0% | 168 ms | 295 ms | 311 ms | 890% | 5.1 MB |
| cpu20_w4 | 4 | 20 us | max | 74,382/s (68,014-109,562) | 76.9% | 217 ms | 328 ms | 360 ms | 1007% | 6.1 MB |
| cpu20_w8 | 8 | 20 us | max | 124,117/s (122,008-172,145) | 45.2% | 242 ms | 377 ms | 459 ms | 1084% | 8.2 MB |
| steady20k_w2 | 2 | 0 us | 20,000/s | 20,013/s (20,011-20,015) | 0.0% | 287 us | 735 us | 1.1 ms | 44% | 4.1 MB |
| steady10k_w2_b1 | 2 | 0 us | 10,000/s | 9,998/s (9,998-9,999) | 0.0% | 115 us | 391 us | 3.6 ms | 179% | 4.0 MB |
| overload_w1 | 1 | 50 us | 40,000/s | 17,899/s (17,576-18,668) | 55.2% | 221 ms | 311 ms | 426 ms | 143% | 4.2 MB |
