# Nor Flash Model
This is C++20 based behavior model for Nor flash. It's targeted for some product flash chips, start with Micro MT25QU02GCBB.
The model requires following external packages:
- Accellera SystemC https://github.com/accellera-official/systemc/archive/refs/tags/3.0.2.tar.gz
- Magic Number https://github.com/Neargye/magic_enum
- Catch2 https://github.com/catchorg/Catch2

## Build
```
mkdir -p build
SYSTEMC_HOME=<path-to-systemc> MAGICNUM_HOME=<path-to-magic_num> make
```

## Run test
```
./build/test_suite 

        SystemC 3.0.2-Accellera --- Jun  9 2026 15:04:00
        Copyright (c) 1996-2025 by all Contributors,
        ALL RIGHTS RESERVED
===============================================================================
All tests passed (362 assertions in 1 test case)
```

## Later Work
Add more flash products like Winbound
