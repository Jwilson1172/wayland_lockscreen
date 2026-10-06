#ifndef PAM_H_DEF
#define PAM_H_DEF

#define PAM_SERVICE_NAME "nyanlock"
#define PAM_SERVICE_FILE "/etc/pam.d/" PAM_SERVICE_NAME

static int check_password(const char *password);

#endif // !PAM_H_DEF
