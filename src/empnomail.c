#include <stdio.h>
#include <stdlib.h>

#define SIZE 1024

int main ( int argc, char * argv [] )
{
  FILE * where, * from ;
  char key[SIZE], data[SIZE];

  if ( (where = fopen ( argv [1], "r" )) == NULL )
    {
      exit (1);
    }

  if ( (from = fopen (argv[2], "r")) == NULL )
    {
      exit (2);
    }

  while (fgets(key, SIZE, where) != 0)
    {
      while (fgets(data, SIZE, from) != 0)
        {
          if (strncmp(key,data,9) == 0)
            {
              printf("%s",data);
              break;
            }
          else
            {
              continue;
            }
        }
    }
}

