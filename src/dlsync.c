/**********************************************************************
 * dlsync (C) M. Brent Harp 2010-2012
 *
 * Synchronize Zimbra distribution lists from LDAP.
 ***********************************************************************/

#include <string.h>

#define _POSIX_SOURCE 1

#include <config.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ldap.h>
#include <syslog.h>
#include <stdarg.h>
#include <limits.h>

char *program_name;
int debug = 0;
int usezmprov = 0;
int delete_shared_folders = 0;
int create_shared_folders = 1;

int alphasort (const void *, const void *);


/*
----------------------------------------------------------------------


                        Zmprov Functions


----------------------------------------------------------------------
*/

#define ZMPROV   "${ZMPROV:-zmprov}>/dev/null" /* environment var or default */

FILE *fpzmprov = NULL;

int
zmprov_open(
 void
){
   if (fpzmprov != NULL)
     return (0);  /* only open one instance */

   if ((fpzmprov = popen (ZMPROV,  "w")) == NULL) {
     fprintf(stderr, 
             "%s: zmprov_open: popen: failed to open '%s'\n",
             program_name,
             ZMPROV);
     return (-1);
   }

   return (0);
}

int
zmprov_close(
  void
){
  if (fpzmprov != NULL) {
    fflush (fpzmprov);
    if (pclose (fpzmprov) == -1) {
      return (-1);
    }
  }
  return (0);
}

int
zmprov_add_dl_member(
  const char *dlname, 
  const char *addr
){
  if (fpzmprov == NULL)
    return (-1);
  
  if (fprintf (fpzmprov, "adlm '%s' '%s'\n", dlname, addr) < 0) {
    return (-1);
  }

  return (0);
}

int
zmprov_remove_dl_member(
  const char *dlname,
  const char *addr
){
  if (fpzmprov == NULL)
    return (-1);

  if (fprintf (fpzmprov, "rdlm '%s' '%s'\n", dlname, addr) < 0) {
    return (-1);
  }

  return (0);
}




/*
  ----------------------------------------------------------------------


                         zmmailbox


  ----------------------------------------------------------------------
*/


#define ZMMAILBOX  "${ZMMAILBOX:-zmmailbox}" /* environment var or default */

FILE *fpzmmailbox = NULL;

int
zmmailbox_open(
 void
){
   if (fpzmmailbox != NULL)
     return (0);  /* only open one instance */

   if ((fpzmmailbox = popen (ZMMAILBOX,  "w")) == NULL) {
     fprintf(stderr,
             "%s: zmmailbox_open: popen: failed to open '%s'\n",
             program_name,
             ZMMAILBOX);
     return (-1);
   }

   return (0);
}

int
zmmailbox_close(
  void
){
  if (fpzmmailbox != NULL) {
    fflush (fpzmmailbox);
    if (pclose (fpzmmailbox) == -1) {
      return (-1);
    }
  }
  return (0);
}


int
zmmailbox_select_mailbox(
  const char *name
){
  if (fpzmmailbox == NULL)
    return (-1);

  if (fprintf (fpzmmailbox, "sm \"%s\"\n", name) < 0) {
    return (-1);
  }

  return (0);
}


int
zmmailbox_create_mountpoint(
  const char *flags,
  const char *path,
  const char *email,
  const char *folder
){
  if (fpzmmailbox == NULL)
    return (-1);

  if (fprintf (fpzmmailbox, "cm -F \"%s\" \"%s\" \"%s\" \"%s\"\n",
               flags, path, email, folder) < 0) {
    return (-1);
  }

  return (0);
}


int
zmmailbox_delete_folder(
  const char *path
){
  if (fpzmmailbox == NULL)
    return (-1);

  if (fprintf (fpzmmailbox, "df \"%s\"\n", path) < 0) {
    return (-1);
  }

  return (0);
}


/*
  ----------------------------------------------------------------------


                         BEncoding


  ----------------------------------------------------------------------
*/

int
B_decode
(
  const char *string,
  const char *format,
  ...)
{
  char *s = (char *)string;
  char *f = (char *)format;
  int   n, slen, nmatch = 0;
  char *c;
  int  *d;
  va_list ap;

  va_start(ap, format);

  while (*s && *f)
  {
    switch (*f++)
    {
      case '%':
        switch (*f++) {
          case 's':        /* match a string */
            if (sscanf(s, "%d:%n", &slen, &n) < 1)
              return -1;
            s += n;
            c = va_arg(ap, char *);
            strncpy(c, s, slen);
            c[slen] = '\0';
            s += slen;
            nmatch++;
            break;

          case 'd':        /* match an int */
            d = va_arg(ap, int *);
            if (sscanf(s, "i%de%n", d, &n) < 1)
              return -1;
            s += n;
            nmatch++;
            break;

          default:         /* match literal */
            if (*s++ != (*(f-1)))
              return -1;
            break;
        }
        break;

      case '{':            /* match a dictionary */
        if (*s++ != 'd')
          return -1;
        break;

      case '[':            /* match a list */
        if (*s++ != 'l')
          return -1;
        break;

      case '}':            /* match end */
      case ']':
        if (*s++ != 'e')
          return -1;
        break;

      default:             /* match literal string */
        f--;
        if (sscanf(s, "%d:%n", &slen, &n) < 1)
          return -1;
        s += n;
        if (strncmp(s, f, slen) != 0)
          return -1;
        s += slen;
        f += slen;
        break;
    }
  }

  va_end(ap);

  if (*f != '\0')
    return -1;

  return nmatch;
}


char *
strrep
(
  char *string,
  char  from,
  char  to
)
{
  char *s;
  for (s = string; *s; s++)
    if (*s == from)
      *s = to;
  return string;
}


/*
  ----------------------------------------------------------------------

  
                        Distribution Lists


  ----------------------------------------------------------------------


  Functions for manipulating Zimbra distribution lists via LDAP. Only
  one list can be operated on at a time. To select a list, call
  dl_select.

*/


#define DL_SUCCESS (0)
#define DL_FAILURE (-1)
#define DL_MAX_FILTER (256)
#define DL_LDAP_LIST_NAME_ATTRIBUTE "zimbraMailAlias"
#define DL_LDAP_MEMBER_ATTRIBUTE "zimbraMailForwardingAddress"
#define DL_LDAP_SHARE_INFO_ATTRIBUTE "zimbraShareInfo"
#define DL_LDAP_URL      "ldap_master_url"
#define DL_LDAP_USERDN   "zimbra_ldap_userdn"
#define DL_LDAP_PASSWORD "zimbra_ldap_password"

enum dl_err {
  DL_ERR_NONE,
  DL_ERR_LDAP,
  DL_ERR_LDAP_CONNECT,
  DL_ERR_LDAP_URL,
  DL_ERR_NO_LIST_SELECTED,
  DL_ERR_UNRECOGNIZED_SYNC_SOURCE,
  DL_ERR_LIST_NOT_FOUND,
  DL_ERR_OUT_OF_MEMORY
};

char *dl_error_messages[] = {
  "no error",
  "LDAP error",
  "LDAP connection error",
  "error parsing LDAP URL",
  "no distribution list selected",
  "unrecognized sync source",
  "list not found",
  "out of memory"
};


char *dl_name        = NULL;                             /* Name of selected list. */
LDAP *dl_ldap        = NULL;                             /* Handle to Zimbra directory. */
char *dl_ldap_url    = "ldap://localhost";               /* Zimbra directory hostname. */
char *dl_ldap_base   = "dc=uoguelph,dc=ca";              /* Zimbra directory search base. */
int   dl_ldap_scope  = LDAP_SCOPE_SUBTREE;               /* Zimbra directory search scope. */
char *dl_ldap_binddn = "uid=zimbra,cn=admins,cn=zimbra"; /* Zimbra admin DN */
char *dl_ldap_passwd = NULL;                             /* Zimbra admin password. */
int   dl_ldap_version = 3;
int   dl_ldap_errno  = 0;
char *dl_ldap_dn     = NULL;                             /* LDAP DN of selected list. */
char *dl_ldap_sync_attribute = "mail";                   /* Default sync attribute. */
int   dl_errno       = 0;                                /* Error code. */
int   dl_share_info_count = 0;                           /* Number of shares. */
char **dl_share_info;                                   /* Share info. */


/**
   dl_perror
   
   Prints the latest error to stderr.
*/
void
dl_perror
(
 const char *msg
)
{
  if (msg != NULL && *msg != '\0') {
    fputs (msg, stderr);
    fputs (": ", stderr);
  }

  fputs (dl_error_messages[dl_errno], stderr);

  fputs ("\n", stderr);

  fflush (stderr);
}



/**
   dl_init

   Initialized module level variables.
*/
int
dl_init 
(
 void
)
{
  int rc;

  if (debug) {
    fprintf (stderr, "Initialize:\n");
  }

  if (dl_cleanup () != DL_SUCCESS)
    return DL_FAILURE;
  
  dl_ldap_url = getenv (DL_LDAP_URL);
  dl_ldap_binddn = getenv (DL_LDAP_USERDN);
  dl_ldap_passwd = getenv (DL_LDAP_PASSWORD);

  if (debug) {
    fprintf (stderr, "  dl_ldap_url = %s\n", dl_ldap_url);
    fprintf (stderr, "  dl_ldap_binddn = %s\n", dl_ldap_binddn);
    fprintf (stderr, "  dl_ldap_passwd = %s\n", dl_ldap_passwd);
  }
  
  /* Connect to LDAP server. */
  rc = ldap_initialize (&dl_ldap, dl_ldap_url);
  if (rc != LDAP_SUCCESS) {
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  ldap_set_option (dl_ldap, LDAP_OPT_PROTOCOL_VERSION, &dl_ldap_version);

  if (ldap_simple_bind_s (dl_ldap, dl_ldap_binddn, dl_ldap_passwd
                          ) != LDAP_SUCCESS) {
    ldap_perror (dl_ldap, program_name);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  return DL_SUCCESS;
}



/**
   dl_cleanup
   
   Frees all module level resources (memory, LDAP connections, etc.)
*/
int
dl_cleanup 
(
 void
)
{
  int status;

  dl_ldap_url = NULL;
  dl_ldap_binddn = NULL;
  dl_ldap_passwd = NULL;

  if (dl_ldap != NULL) {
    status = ldap_unbind (dl_ldap);
    if (status != LDAP_SUCCESS)
      return DL_FAILURE;
    dl_ldap = NULL;
  }

  if (dl_ldap_dn != NULL) {
    ldap_memfree (dl_ldap_dn);
    dl_ldap_dn = NULL;
  }

  return DL_SUCCESS;
}



/**
   dl_select

   Select a distribution list by name. The selected DL is the implicit
   target for all further operations, until a new list is selected. On
   success, the global variable dl_ldap_dn is set.

   Return 0 on success. On error, set dl_errno appropriately and
   return -1.
*/
int 
dl_select 
(
 char *name
)
{
  LDAPMessage *result;
  LDAPMessage *entry;
  char        filter[DL_MAX_FILTER+1];
  char        *attrs[] = { "dn", "zimbraShareInfo", NULL };
  char       **values;
  int         status;
  int         i;

  if (debug) {
    fprintf (stderr, "Select distribution list:\n");
    fprintf (stderr, "  name = %s\n", name);
  }

  /* Free the name from a previous call. */
  if (dl_name != NULL) {
    free (dl_name);
    dl_name = NULL;
  }

  /* Save the DL name. */
  dl_name = strdup (name);

  /* Free the DN from a previous call. */
  if (dl_ldap_dn != NULL) {
    ldap_memfree (dl_ldap_dn);
    dl_ldap_dn = NULL;
  }

  /* Free share info from a previous call. */
  if (dl_share_info != NULL) {
    for (i = 0; i < dl_share_info_count; i++) {
      if (dl_share_info[i] != NULL) {
        free (dl_share_info[i]);
        dl_share_info[i] = NULL;
      }
    }
    free(dl_share_info);
    dl_share_info = NULL;
    dl_share_info_count = 0;
  }

  sprintf(filter, "(%s=%s)", DL_LDAP_LIST_NAME_ATTRIBUTE, name);

  if (debug) {
    fprintf (stderr, "Search for DL entry:\n");
    fprintf (stderr, "  dl_ldap_base = %s\n", dl_ldap_base);
    fprintf (stderr, "  dl_ldap_scope = %d\n", dl_ldap_scope);
    fprintf (stderr, "  filter = %s\n", filter);
    fprintf (stderr, "  attrs = %s\n", attrs[0]);
  }

  status = ldap_search_s 
    (dl_ldap,
     dl_ldap_base,
     dl_ldap_scope,
     filter,
     attrs,
     0,
     &result);
  
  if (status != LDAP_SUCCESS) {
    ldap_perror (dl_ldap, program_name);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }
    
  /* Get the list's DN. */
  entry = ldap_first_entry (dl_ldap, result);

  if (entry == NULL) {
    if (debug) {
      fprintf (stderr, "  %s: dl not found\n", name);
    }
    result && ldap_msgfree (result);
    dl_errno = DL_ERR_LIST_NOT_FOUND;
    return DL_FAILURE;
  }

  dl_ldap_dn = ldap_get_dn (dl_ldap, entry);

  if (dl_ldap_dn == NULL) {
    if (debug) {
      fprintf (stderr, "  %s: dl not found\n", name);
    }
    result && ldap_msgfree (result);
    dl_errno = DL_ERR_LIST_NOT_FOUND;
    return DL_FAILURE;
  }

  if (debug) {
    fprintf (stderr, "Copy share info strings.\n");
  }
  values = (char **)ldap_get_values (dl_ldap, entry, DL_LDAP_SHARE_INFO_ATTRIBUTE);
  dl_share_info_count = ldap_count_values (values);
  dl_share_info = calloc((dl_share_info_count + 1), sizeof(char*));
  if (dl_share_info == NULL) {
    dl_errno = DL_ERR_OUT_OF_MEMORY;
    return -1;
  }
  for (i = 0; i < dl_share_info_count; i++) {
    if ((dl_share_info[i] = strdup(values[i])) == NULL) {
      dl_errno = DL_ERR_OUT_OF_MEMORY;
      return -1;
    }
    if (debug) {
      fprintf(stderr, "ShareInfo[%d]: %s\n", i, dl_share_info[i]);
    }
  }
  ldap_value_free(values);

  if (debug) {
    fprintf (stderr, "  return %s\n", dl_ldap_dn);
  }

  /* Clean up. */
  if (result != NULL)
    ldap_msgfree (result);

  return DL_SUCCESS;
}



/**
   dl_remove_members

   Removes members from the current distribution list.  Returns
   true if the members are removed, false if an error occurs.  In the case
   of an error, dl_errno is set appropriately.
*/
int
dl_remove_members 
(
 char **mail
)
{
  LDAPMod     *mods[2], mod;
  
  if (dl_ldap_dn == NULL) {
    dl_errno = DL_ERR_NO_LIST_SELECTED;
    return DL_FAILURE;
  }

  mod.mod_op = LDAP_MOD_DELETE;
  mod.mod_type = DL_LDAP_MEMBER_ATTRIBUTE;
  mod.mod_values = mail;
  mods[0] = &mod;
  mods[1] = NULL;

  if (ldap_modify_s (dl_ldap, dl_ldap_dn, 
                     mods) != LDAP_SUCCESS) {
    ldap_perror (dl_ldap, program_name);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  return 0;
}



/**
   dl_add_members

   Adds a list of members to the current distribtion list.
*/
int
dl_add_members
(
 char **mail
)
{
  LDAPMod     *mods[2], mod;
  char        *modvals[2];
  int         share_index;

  if (dl_ldap_dn == NULL) {
    dl_errno = DL_ERR_NO_LIST_SELECTED;
    return DL_FAILURE;
  }

  mod.mod_op = LDAP_MOD_ADD;
  mod.mod_type = DL_LDAP_MEMBER_ATTRIBUTE;
  mod.mod_values = mail;
  mods[0] = &mod;
  mods[1] = NULL;

  if (ldap_modify_s (dl_ldap, dl_ldap_dn,
                     mods) != LDAP_SUCCESS) {
    ldap_perror (dl_ldap, program_name);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  /* Mount published shares. */
  for (share_index = 0; share_index < dl_share_info_count; share_index++) {
    char path[512], *share_info, *owner_id, *folder_id, *share_data;
    char disp[256], email[256], fldr[256], *s, **m;
    int  view;
    share_info = strdup(dl_share_info[share_index]);
    owner_id   = strtok(share_info, ";");
    folder_id  = strtok(NULL, ";");
    share_data = strtok(NULL, ";");
    B_decode(share_data, "[{d%se%sf%sv%d}", disp, email, fldr, &view);
    snprintf(path, 512, "/%s's %s", disp, fldr+1);
    strrep(path, '\"', '\'');
    for (m = mail; *m; m++) {
      zmmailbox_select_mailbox(*m);
      if (delete_shared_folders)
        zmmailbox_delete_folder(path);
      if (create_shared_folders)
        zmmailbox_create_mountpoint("#", path, email, fldr); 
    }
    free(share_info);
  }
     
  return DL_SUCCESS;
}


int
dl_add_member
(
  const char *addr
)
{
  char *dl_get_name();
  if (usezmprov) {
    zmprov_add_dl_member(dl_get_name(), addr);
  }
}


int
dl_remove_member
(
  const char *addr
)
{
  char *dl_get_name();
  if (usezmprov) {
    zmprov_add_dl_member(dl_get_name(), addr);
  }
}



char**
dl_get_members
(
 void
)
{
  char        **members;
  char        **values;
  LDAPMessage *result;
  LDAPMessage *entry;
  char        filter[DL_MAX_FILTER+1];
  char        *attrs[] = { DL_LDAP_MEMBER_ATTRIBUTE, NULL };
  int         state;
  int         count;
  int         index;

  if (dl_ldap_dn == NULL) {
    dl_errno = DL_ERR_NO_LIST_SELECTED;
    return NULL;
  }

  sprintf(filter, "(%s=%s)", DL_LDAP_LIST_NAME_ATTRIBUTE, dl_name);

  if (debug) {
    fprintf (stderr, "Get DL Members:\n");
    fprintf (stderr, "  filter='%s'\n", filter);
  }

  /* Search for entries matching filter. */
  state = ldap_search_s(
     dl_ldap,                  /* LDAP handle */
     dl_ldap_base,             /* search base */
     dl_ldap_scope,            /* search scope */
     filter,                   /* search filter */
     attrs,                    /* search attributes */
     0,                        /* attrs only */
     &result);

  if (state != LDAP_SUCCESS) {
    ldap_perror (dl_ldap, program_name);
    dl_errno = DL_ERR_LDAP;
    return NULL;
  }

  /* there can only be one match */
  entry = ldap_first_entry (dl_ldap, result);

  if (entry == NULL) {
    dl_errno = DL_ERR_LIST_NOT_FOUND;
    return NULL;
  }
    
  values = (char **)ldap_get_values (dl_ldap, entry, DL_LDAP_MEMBER_ATTRIBUTE);
  count = ldap_count_values (values);
  members = calloc (count + 1, sizeof (char*));
  while (--count >= 0) {
    if ((members[count] = strdup (values[count])) == NULL) {
      dl_errno = DL_ERR_OUT_OF_MEMORY;
      ldap_value_free (members);
      free (members);
      return NULL;
    }
    if (debug) {
      fprintf (stderr, "  member='%s'\n", members[count]);
    }
  }
  ldap_value_free (values);
  ldap_msgfree (result);

  return members;
}

/**
   dl_get_name

   Returns the name of the currently selected list.
*/
char *
dl_get_name
(
 void
)
{
  if (dl_ldap_dn == NULL) {
    dl_errno = DL_ERR_NO_LIST_SELECTED;
    return NULL;
  }

  return dl_name;
}


int
set_difference
(
  void *set1, int n1,
  void *set2, int n2,
  void *result_set, size_t sz,
  int (*compare)(const void *, const void *)
)
{
  void *first1, *first2, *last1, *last2, *result;

  first1 = set1;
  first2 = set2;
  last1 = first1 + n1 * sz;
  last2 = first2 + n2 * sz;
  result = result_set;

  while ( first1 != last1 && first2 != last2 )
    {
      int d;

      if ((d = compare(first1, first2)) < 0)
         {
           memcpy(result, first1, sz);
           result += sz;
           first1 += sz;
         }
      else if (d > 0)
         {
           first2 += sz;
         }
      else
         {
           first1 += sz;
           first2 += sz;
         }
    }
  
  while (first1 != last1)
    {
      memcpy(result, first1, sz);
      result += sz;
      first1 += sz;
    }

  return ((result - result_set) / sz);
}

/**
   difference

   Stores the elements of list1 that are not in list2 in diff,
   and returns the number of elements in diff.
*/
int
difference
(
  void   *diff,
  void   *list1,
  size_t  n1,
  void   *list2,
  size_t  n2,
  size_t  size,
  int   (*compar)(const void *, const void *)
)
{
  int i1 = 0, i2 = 0, m = 0, r;

  while (i1 < n1 && i2 < n2)
    {
      r = compar (list1 + i1 * size, list2 + i2 * size);

      if (r < 0)
        {
          memcpy(diff + m++ * size, list1 + i1++ * size, size);
        }
      else if (r > 0)
        {
          i2++;
        }
      else
        {
          i1++;
          i2++;
        }
    }

  while (i1 < n1)
    {
      memcpy(diff + m++ * size, list1 + i1++ * size, size);
    }

  return m;
}


int
copy_attribute
(
  LDAP *ld,
  LDAPMessage *res,
  const char *attribute,
  char **result
)
{
  LDAPMessage *ent;
  char **values;
  int n = 0;
  for (ent = ldap_first_entry(ld, res); ent != NULL; ent = ldap_next_entry(ld, ent))
    if ((values = (char **)ldap_get_values(ld, ent, attribute)) && values[0])
      if ((result[n++] = strdup(values[0])) == NULL)
        return -1;
  return n;
}
      

/**
   dl_ldap_sync

   Replaces list membership with results of the given LDAP query.  The
   'mail' attribute is added as a member for every search result.
   Returns the number of members added to the list, or -1 if an error
   occured.  In the event of an error, ld_errno is set appropriately.
*/
int
dl_ldap_sync
(
 const char *url,
 const char *mail,
 const char *binddn,
 const char *passwd
)
{
  LDAP        *ld  = 0;
  LDAPURLDesc *lud = 0;
  LDAPMessage *res, *entry;
  char        **values, **value;
  char        **members, **matches, **add, **del;
  int         m = 0, n = 0, n_del = 0, n_add = 0;  /* Counters */
  int         count = 0;     /* Number of entries returned. */
  int         state;
  int         v3 = 3;

  state = ldap_url_parse (url, &lud);

  if (state != 0) {
    dl_errno = DL_ERR_LDAP_URL;
    return -1;
  }
  
  if (debug) {
    fprintf (stderr, "Connect to LDAP server:\n");
    fprintf (stderr, "  lud->lud_host = %s\n", lud->lud_host);
    fprintf (stderr, "  lud->lud_port = %d\n", lud->lud_port);
  }

  /* Connect to the LDAP server. */
  ld = (LDAP *)ldap_init (lud->lud_host, lud->lud_port);
  if (ld == NULL) {
    ldap_free_urldesc (lud);
    dl_errno = DL_ERR_LDAP_CONNECT;
    return -1;
  }

  if (debug) {
    fprintf (stderr, "Set protocol version: %d\n", v3);
  }

  /* Use LDAP v3 */
  ldap_set_option (ld, LDAP_OPT_PROTOCOL_VERSION, &v3);

  if (debug) {
    fprintf (stderr, "Start TLS.\n");
  }

  /* Use TLS */
  if (ldap_start_tls_s (ld, NULL, NULL)
      != LDAP_SUCCESS) {
    ldap_perror (ld, program_name);
    ldap_free_urldesc (lud);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  if (debug) {
    fprintf (stderr, "LDAP simple bind:\n");
    fprintf (stderr, "  binddn = %s\n", binddn);
    fprintf (stderr, "  passwd = %s\n", passwd);
  }

  /* Bind */
  if (ldap_simple_bind_s (ld, binddn, passwd
                          ) != LDAP_SUCCESS) {
    ldap_perror (ld, program_name);
    ldap_free_urldesc (lud);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  if (debug) {
    fprintf (stderr, "Search for entries matching filter:\n");
    fprintf (stderr, "  lud->lud_dn = %s\n", lud->lud_dn);
    fprintf (stderr, "  lud->lud_scope = %d\n", lud->lud_scope);
    fprintf (stderr, "  lud->lud_filter = %s\n", lud->lud_filter);
    //fprintf (stderr, "  lud->lud_attrs = %s\n", lud->lud_attrs);
  }
             
  /* Search for entries matching filter. */
  state = ldap_search_s (ld,
                         lud->lud_dn,
                         lud->lud_scope,
                         lud->lud_filter, 
                         lud->lud_attrs,
                         0,
                         &res);

  if (state != LDAP_SUCCESS) {
    ldap_perror (ld, program_name);
    ldap_unbind (ld);
    ldap_free_urldesc (lud);
    dl_errno = DL_ERR_LDAP;
    return DL_FAILURE;
  }

  /* Allocate space for matching addresses. */
  n = ldap_count_entries (ld, res);
  if ((matches = calloc(n+1, sizeof(char *))) == NULL)
    err(EXIT_FAILURE);
  if ((n = copy_attribute(ld, res, mail, matches)) < 0)
    err(EXIT_FAILURE);

  /* Get current members. */
  members = dl_get_members ();
  m = ldap_count_values (members);

  /* Sort alphabetically. */
  qsort (members, m, sizeof(char*), alphasort);
  qsort (matches, n, sizeof(char*), alphasort);

  /* Allocate space for add and delete lists. */
  del = calloc (m + 1, sizeof (char *));
  add = calloc (n + 1, sizeof (char *));

  /* Compute add and delete lists. */
  n_del = set_difference (members, m, matches, n, del, sizeof (char *), alphasort);
  n_add = set_difference (matches, n, members, m, add, sizeof (char *), alphasort);
  
  if (debug)
    {
      int i;
      fprintf (stderr, "Members to delete:\n");
      for (i = 0; i < n_del; i++)
        fprintf (stderr, "  %s\n", del[i]);
      fprintf (stderr, "Members to add:\n");
      for (i = 0; i < n_add; i++)
        fprintf (stderr, "  %s\n", add[i]);
    }

  /* Update DL */
  if (n_del > 0) dl_remove_members (del);
  if (n_add > 0) dl_add_members (add);

  /* Cleanup */
  while (m) free (members[--m]);
  while (n) free (matches[--n]);

  if (members) free (members);
  if (matches) free (matches);
  if (del) free (del);
  if (add) free (add);

  ldap_msgfree (res);
  ldap_unbind (ld);
  ldap_free_urldesc (lud);
  
  return (n_add - n_del);
}







/**
   dl_sync

   Replaces list membership from an external source.
*/
int
dl_sync 
(
 char *name, 
 char *source,
 char *binddn,
 char *passwd
)
{
  if (debug) {
    fprintf (stderr, "Synchronize DL:\n");
    fprintf (stderr, "  name = %s\n", name);
    fprintf (stderr, "  source = %s\n", source);
    fprintf (stderr, "  binddn = %s\n", binddn);
    fprintf (stderr, "  passwd = %s\n", passwd);
  }
  
  if (dl_select (name) != DL_SUCCESS) {
    return DL_FAILURE;
  }

  if (ldap_is_ldap_url (source)) {
    int count = dl_ldap_sync (source, dl_ldap_sync_attribute, binddn, passwd);
    if (count < 0)
      return DL_FAILURE;
    printf ("%s %d\n", name, count);
    return DL_SUCCESS;
  }

  dl_errno = DL_ERR_UNRECOGNIZED_SYNC_SOURCE;
  return DL_FAILURE;
}








/*
----------------------------------------------------------------------


                         Local Functions


----------------------------------------------------------------------
*/


extern char *optarg;
extern int   optind;

char *binddn = NULL;
char *passwd = NULL;

void
usage 
(
 void
)
{
  fprintf(stderr,
	  "Usage: %s [dlname ldapurl]*\n"
	  "\n"
	  "\tSynchronizes Zimbra distribution lists with\n"
          "\tan external LDAP source.\n"
	  "\n"
	  "Options:\n"
          "  -D           LDAP source bind DN\n"
          "\n"
          "  -w           LDAP source bind password\n"
          "\n"
          "  -Y           LDAP source password file\n"
          "\n"
          "  -d           Debug mode\n"
          "\n"
          "  -B           Use zmprov\n"
          "\n"
	  "  -h           Display this help message\n"
	  "\n"
	  ,program_name);
}


char*
readpw
(
 char *path
 )
{
  FILE *file;
  char pwd[32];
  
  file = fopen (path, "r");
  if (file == NULL) {
    return NULL;
  }
  if (fgets (pwd, 32, file))
    return strdup(pwd);
  else
    return NULL;
}


int
main 
(
 int argc,
 char *argv[]
)
{
  int errcount = 0;
  char *s;

  program_name = argv[0];

  while (--argc > 0 && (*++argv)[0] == '-') {
    for (s = argv[0]+1; *s != '\0'; s++)
      switch (*s) {
      case 'D':                 /* LDAP source bind DN */
        binddn = *++argv;
        --argc;
        break;
      case 'w':                 /* LDAP source bind Password */
        passwd = *++argv;
        --argc;
        break;
      case 'Y':                 /* LDAP source password file */
        passwd = readpw (*++argv);
        --argc;
      case 'd':                 /* Toggle debug mode */
        debug = !debug;
        break;
      case 'B':                 /* Use zmprov */
        usezmprov = !usezmprov;
        break;
      case 'r':                 /* Remove shared folders. */
        delete_shared_folders = !delete_shared_folders;
        break;
      case 'n':                 /* Do not create shared folders. */
        create_shared_folders = !create_shared_folders;
        break;
      }
  }

  if (argc < 2) {
    usage();
    exit(EXIT_FAILURE);
  }

  if (dl_init () != DL_SUCCESS) {
    dl_perror ("dl_init");
    exit (EXIT_FAILURE);
  }
  
  if (usezmprov) {
    zmprov_open ();
  }

  if (zmmailbox_open () != 0) {
    fprintf (stderr, "failed to open zmmailbox\n");
    exit (EXIT_FAILURE);
  }

  while (argc > 1) {
    if (dl_sync (argv[0], argv[1], binddn, passwd) != DL_SUCCESS) {
      dl_perror (program_name);
      errcount++;
    }
    argv += 2;
    argc -= 2;
  }
  
  dl_cleanup ();
  
  if (usezmprov) {
    zmprov_close ();
  }

  if (zmmailbox_close () != 0) {
    fprintf (stderr, "warning: failed to close zmmailbox\n");
  }

  exit (errcount == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}


int
alphasort
(
 const void *p1,
 const void *p2
)
{
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

