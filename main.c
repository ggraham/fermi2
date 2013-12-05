#include <stdio.h>
#include <string.h>

#define FM_VERSION "r69"

int main_diff(int argc, char *argv[]);
int main_sub(int argc, char *argv[]);
int main_unpack(int argc, char *argv[]);
int main_correct(int argc, char *argv[]);
int main_count(int argc, char *argv[]);
int main_diff2(int argc, char *argv[]);
int main_inspectk(int argc, char *argv[]);

void liftrlimit(void);
double cputime(void);
double realtime(void);

int main(int argc, char *argv[])
{
	int ret = 0, i;
	double t_start;
	liftrlimit();
	if (argc == 1) {
		fprintf(stderr, "\n");
		fprintf(stderr, "Program: fermi2\n");
		fprintf(stderr, "Version: %s\n", FM_VERSION);
		fprintf(stderr, "Contact: http://hengli.uservoice.com/\n\n");
		fprintf(stderr, "Usage:   fermi2 <command> [arguments]\n\n");
		fprintf(stderr, "Command: diff     compare two FMD-indices\n");
		fprintf(stderr, "         occflt   pick up reads containing low-occurrence k-mers\n");
		fprintf(stderr, "         sub      subset FM-index\n");
		fprintf(stderr, "         unpack   unpack FM-index\n");
		fprintf(stderr, "         correct  error correction\n");
		fprintf(stderr, "         count    k-mer counting (inefficient)\n");
		fprintf(stderr, "\n");
		return 1;
	}
	t_start = realtime();
	if (strcmp(argv[1], "diff") == 0) ret = main_diff(argc-1, argv+1);
	else if (strcmp(argv[1], "occflt") == 0) ret = main_diff(argc-1, argv+1);
	else if (strcmp(argv[1], "diff2") == 0) ret = main_diff2(argc-1, argv+1);
	else if (strcmp(argv[1], "sub") == 0) ret = main_sub(argc-1, argv+1);
	else if (strcmp(argv[1], "unpack") == 0) ret = main_unpack(argc-1, argv+1);
	else if (strcmp(argv[1], "correct") == 0) ret = main_correct(argc-1, argv+1);
	else if (strcmp(argv[1], "count") == 0) ret = main_count(argc-1, argv+1);
	else if (strcmp(argv[1], "inspectk") == 0) ret = main_inspectk(argc-1, argv+1);
	else {
		fprintf(stderr, "[E::%s] unknown command\n", __func__);
		return 1;
	}
	if (ret == 0) {
		fprintf(stderr, "[M::%s] Version: %s\n", __func__, FM_VERSION);
		fprintf(stderr, "[M::%s] CMD:", __func__);
		for (i = 0; i < argc; ++i)
			fprintf(stderr, " %s", argv[i]);
		fprintf(stderr, "\n[M::%s] Real time: %.3f sec; CPU: %.3f sec\n", __func__, realtime() - t_start, cputime());
	}
	return ret;
}
