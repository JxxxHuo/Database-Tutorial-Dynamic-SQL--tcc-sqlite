#include <stdio.h>
#include "sqlite3.h"


int main(void){
   
    int tt=12;
    printf("%d\n",tt);
    printf("%s\n",sqlite3_libversion());
   
    return 0;
   
}