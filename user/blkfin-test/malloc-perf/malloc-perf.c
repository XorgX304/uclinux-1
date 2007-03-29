#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>


int main(int argc, char* argv[]) {
  unsigned int i, * j, k, a, t1, t2, max, ave, min;
  struct timeval tim1, tim2;

  k = 4 ;
  for (i = 0; i <= 16 * 1024 ; i+=k ) {
        max = 0;
        ave = 0;
        min = 0xFFFFFFFF;
        for (a = 0; a < 128 ; a ++ ) {
                gettimeofday(&tim2, NULL);
                j = (int *)malloc (i*1024 - sizeof (size_t));
                gettimeofday(&tim1, NULL);
                free (j);
                t1 =   (tim1.tv_sec-tim2.tv_sec) * 1000000 + tim1.tv_usec-tim2.tv_usec;
                if ( max < t1 ) max = t1;
                if ( min > t1 ) min = t1;
                ave = ave + t1;
        }
        printf("%05ik : 0x%08x  %06i %06i %06i\n", i, j, min, ave/128, max);
        k = 1024;
        if ( i < 1024 ) k = 128;
        if ( i < 128 ) k = 4;
  }

  return 0;
}
