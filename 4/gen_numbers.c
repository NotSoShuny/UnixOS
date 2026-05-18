#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *out_path = "numbers.txt";
    FILE *out_file;
    long sum = 0;
    long value;
    int i;

    if (argc > 1) {
        out_path = argv[1];
    }

    srand((unsigned int)time(NULL));

    out_file = fopen(out_path, "w");
    if (out_file == NULL) {
        perror("fopen");
        return 1;
    }

    for (i = 0; i < 1000 - 1; i++) {
        value = (rand() % 1001) - 500; // Число от -500 до 500
        sum += value;
        fprintf(out_file, "%ld\n", value);
    }

    fprintf(out_file, "%ld\n", -sum);

    fclose(out_file);
    return 0;
}