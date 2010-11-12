#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <values.h>
#include <string.h>
#include <time.h>
#include <sys/dir.h>

#include "ui.h"
#include "info.h"
#include "btree.h"

int main(int argc, char **argv)
{
	DIR *dirp;
	struct dirent *direp = NULL;
	char *outfile = NULL;

	ui *userinfo = &btree_ui;
//	ui *userinfo = &bthread_ui;

	if (argc < 3) {
		printf("%s <user_info.csv> <path> [out.csv]\n", argv[0]);
		return -1;
	}
	if (argc == 4)
		outfile = argv[3];

	ui_init(userinfo);
	ui_addfile(userinfo, argv[1]);
#if 0
	{
		char *key, str[256];
		FILE *fp  =  fopen(argv[1], "r");
		while (!feof(fp)) {
			memset(str, 0, 256);

			if (fgets(str, 255, fp) == NULL)
				break;
			key = strtok(str, ",");
			if (key == NULL)
				break;

			if (ui_find(userinfo, key) == 0)
				printf("not found %s\n", key);
		}
		fclose(fp);
	}
#endif

#if 0
	dirp = opendir(argv[2]);

	if (dirp) {
		char filename[256];
		direp = readdir(dirp);
		for (; direp != NULL; direp = readdir(dirp)) {
			if (!strcmp(direp->d_name, ".") || !strcmp(direp->d_name, ".."))
				continue;
		
			sprintf(filename, "%s/%s", argv[2], direp->d_name);
			ui_addfile(userinfo, filename);
		}

		closedir(dirp);
	}
#endif
	ui_out(userinfo, outfile);
	ui_free(userinfo);

	return  0;
}

