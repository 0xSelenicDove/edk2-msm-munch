#include <stdio.h>

int main(int argc, char **argv)
{
  printf("argc=%d\n", argc);
  for (int index = 0; index < argc; ++index) {
    printf("argv[%d]=<%s>\n", index, argv[index]);
  }
  return 0;
}
