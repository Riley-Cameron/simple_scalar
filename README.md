# Simple Scalar

Run Default OOO Command (from repo root dir):
```
./Run.pl -db ./bench.db -dir results/gcc1 -benchmark gcc -sim $PWD/ss3/sim-outorder -args "-fastfwd 20000000 -max:inst 20000000 -cache:dl1 dl1:64:64:4:l -cache:dl2 dl2:1024:64:4:l -cache:il2 il2:1024:32:1:l -mem:lat 300 2 -cache:dl1lat 3 -cache:dl2lat 30"
```

Run BDI Cache Command
```
./Run.pl -db ./bench.db -dir results/gcc1 -benchmark gcc -sim $PWD/ss3/sim-outorder -args "-fastfwd 20000000 -max:inst 20000000 -cache:dl1 dl1:64:64:4:l -cache:dl2 dl2:1024:64:4:b -cache:il2 il2:1024:32:1:l -mem:lat 300 2 -cache:dl1lat 3 -cache:dl2lat 30"
```

## Working Benchmarks:
- gcc
- li
- go
- ijpeg
- perl