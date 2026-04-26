#ifndef CMDLINE_AFP_H_
#define CMDLINE_AFP_H_

extern int full_url;

int com_dir(char *arg);
int com_chmod(char *arg);
int com_put(char *filename);
int com_get(char *filename);
int com_view(char *arg);
int com_rename(char *arg);
int com_copy(char *arg);
int com_delete(char *arg);
int com_mkdir(char *arg);
int com_rmdir(char *arg);
int com_lcd(char *path);
int com_cd(char *path);
int com_touch(char *path);
int com_lpwd(char *unused);
int com_pwd(char *unused);
int com_status(char *unused);
int com_statvfs(char *unused);
int com_pass(char *unused);
int com_disconnect(char *unused);
int com_exit(char *unused);

void cmdline_afp_exit(void);
int cmdline_afp_setup(int recursive, int batch_mode, char * url_string);
void cmdline_afp_setup_client(void);
void cmdline_set_log_level(int loglevel);
void cmdline_set_verbose(int verbose);
void cmdline_set_dsi_timeout(int timeout);
int cmdline_batch_transfer(char * local_path, int direction, int recursive);
char *afp_remote_file_generator(const char *text, int state);

#define ARG_LEN 1024
#define MAX_INPUT_LEN 4096  /* Maximum length for extended input (URLs, long paths) */

#endif
