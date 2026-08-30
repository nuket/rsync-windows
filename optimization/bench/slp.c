#include <windows.h>
#include <stdio.h>
int main(void){
  LARGE_INTEGER f,a,b; QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  for(int i=0;i<200;i++) Sleep(1);
  QueryPerformanceCounter(&b);
  printf("Sleep(1) avg = %.3f ms\n", 1000.0*(b.QuadPart-a.QuadPart)/f.QuadPart/200.0);
  timeBeginPeriod(1);
  QueryPerformanceCounter(&a);
  for(int i=0;i<200;i++) Sleep(1);
  QueryPerformanceCounter(&b);
  printf("Sleep(1) avg with timeBeginPeriod(1) = %.3f ms\n", 1000.0*(b.QuadPart-a.QuadPart)/f.QuadPart/200.0);
  return 0;
}
