# Database-Tutorial-Dynamic-SQL--tcc-sqlite

this is a tutorial experiment for students to practice dynamic sql based on sqlite in Windows system.

There are two methods available here, one is to use TCC compiler as in dos_tools folder, the other is to use Visual Studio as in VC_example folder.

The code in TCC dos_tools folder is standard C and the code in VC_example is C++.

There is no need to install any extra software if you do not have compilers. The introduction about TCC compiler is in [Fabrice Bellard' website](https://bellard.org/tcc/), which is a portable, tiny and fast C compiler less than 2M. 

The introduction about sqlite API for C is in [An Introduction To The SQLite C/C++ Interface](https://www.sqlite.org/cintro.html)

In dos_tools folder, the example code like select.c,create.c,insert.c in dos_tools folder show how to execute sql in C.

To see the query result in dos windows simply type in command:

```
C:\lab>tcc.exe sqlite3.dll -run select.c
```


