#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAM_SERVICE_NAME "nyanlock"
#define PAM_SERVICE_FILE "/etc/pam.d/" PAM_SERVICE_NAME

// Handles the pam conversation
// Since our app is just dealing with the local user login context we can safely
// assume that the first PAM_PROMPT_ECHO_OFF msg we get is probably(%95) a
// password field. COULDDO: Support HSM/FIDO logins that'd be neat.
static int pam_conv_fn(int num_msg, const struct pam_message **msg,
                       struct pam_response **response, void *data)
{
        const char *password = data;
        if (num_msg <= 0) {
                return PAM_CONV_ERR;
        }
        struct pam_response *r = calloc((size_t)num_msg, sizeof(*r));
        if (!r) {
                return PAM_BUF_ERR;
        }
        for (int i = 0; i < num_msg; i++) {
                if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF) {
                        continue;
                }
                r[i].resp = strdup(password);
                // deconstruct message safe wiping the secret on fail.
                if (!r[i].resp) {
                        for (int j = 0; j < i; j++) {
                                if (r[j].resp) {
                                        explicit_bzero(r[j].resp,
                                                       strlen(r[j].resp));
                                        free(r[j].resp);
                                }
                        }
                        free(r);
                        return PAM_BUF_ERR;
                }
        }
        *response = r;
        return PAM_SUCCESS;
}
// Returns 1 if 'password' is correct for the current user, else 0
static int check_password(const char *password)
{
        struct passwd *pw = getpwuid(getuid());
        if (!pw) {
                return 0;
        }
        struct pam_conv conv = { pam_conv_fn, (void *)password };
        pam_handle_t   *pamh = NULL;

        int ret = pam_start(PAM_SERVICE_NAME, pw->pw_name, &conv, &pamh);
        if (ret != PAM_SUCCESS) {
                return 0;
        }

        ret = pam_authenticate(pamh, 0);
        pam_end(pamh, ret);
        return ret == PAM_SUCCESS;
}
