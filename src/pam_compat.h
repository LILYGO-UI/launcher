#ifndef LILYGO_UI_LAUNCHER_PAM_COMPAT_H
#define LILYGO_UI_LAUNCHER_PAM_COMPAT_H

/*
 * The CM0 runtime SDK contains libpam but intentionally does not ship PAM
 * development headers.  These are the stable ABI declarations used by the
 * launcher; keeping them local lets the cross build link the target runtime
 * without copying a system header into the application.
 */
typedef struct pam_handle pam_handle_t;

struct pam_message {
    int msg_style;
    const char *msg;
};

struct pam_response {
    char *resp;
    int resp_retcode;
};

struct pam_conv {
    int (*conv)(int, const struct pam_message **, struct pam_response **, void *);
    void *appdata_ptr;
};

extern "C" {
int pam_start(const char *, const char *, const struct pam_conv *, pam_handle_t **);
int pam_end(pam_handle_t *, int);
int pam_set_item(pam_handle_t *, int, const void *);
int pam_authenticate(pam_handle_t *, int);
int pam_acct_mgmt(pam_handle_t *, int);
int pam_setcred(pam_handle_t *, int);
int pam_open_session(pam_handle_t *, int);
int pam_close_session(pam_handle_t *, int);
}

enum {
    PAM_SUCCESS = 0,
    PAM_BUF_ERR = 5,
    PAM_CONV_ERR = 19,
    PAM_PROMPT_ECHO_OFF = 1,
    PAM_PROMPT_ECHO_ON = 2,
    PAM_ERROR_MSG = 3,
    PAM_TEXT_INFO = 4,
    PAM_TTY = 3,
    PAM_ESTABLISH_CRED = 0x4000,
    PAM_DELETE_CRED = 0x4001,
};

#endif
