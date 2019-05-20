#include "adiak.h"
#include "adiak_internal.h"
#include "adiak_tool.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

typedef struct adiak_tool_t {
   //Below fields are present in v1
   int version;
   struct adiak_tool_t *next;
   struct adiak_tool_t *prev;
   void *opaque_val;
   adiak_nameval_cb_t name_val_cb;
   int report_on_all_ranks;
   adiak_category_t category;
} adiak_tool_t;

static adiak_t *global_adiak;
static adiak_tool_t **tool_list;

static int measure_adiak_walltime;
static int measure_adiak_systime;
static int measure_adiak_cputime;

static adiak_datatype_t base_long = { adiak_long, adiak_rational, 0, 0, NULL };
static adiak_datatype_t base_ulong = { adiak_ulong, adiak_rational, 0, 0, NULL };
static adiak_datatype_t base_int = { adiak_int, adiak_rational, 0, 0, NULL };
static adiak_datatype_t base_uint = { adiak_uint, adiak_rational, 0, 0, NULL };
static adiak_datatype_t base_double = { adiak_double, adiak_rational, 0, 0, NULL };
static adiak_datatype_t base_date = { adiak_date, adiak_interval, 0, 0, NULL };
static adiak_datatype_t base_timeval = { adiak_timeval, adiak_interval, 0, 0, NULL };
static adiak_datatype_t base_version = { adiak_version, adiak_ordinal, 0, 0, NULL };
static adiak_datatype_t base_string = { adiak_string, adiak_ordinal, 0, 0, NULL };
static adiak_datatype_t base_catstring = { adiak_catstring, adiak_categorical, 0, 0, NULL };
static adiak_datatype_t base_path = { adiak_path, adiak_categorical, 0, 0, NULL };

static void adiak_common_init();

static int format_match(const char *users, const char *reference);
static int adiak_nameval(const char *name, const void *buffer, size_t buffer_size, size_t elem_size, adiak_datatype_t valtype);

static void adiak_register(int adiak_version, adiak_category_t category,
                           adiak_nameval_cb_t nv,
                           int report_on_all_ranks, void *opaque_val);

adiak_t* adiak_globals()
{
   return global_adiak;
}

static int find_end_brace(const char *typestr, char endchar, int typestr_start, int typestr_end);
static adiak_datatype_t *parse_typestr(const char *typestr, va_list *ap);
static adiak_datatype_t *parse_typestr_helper(const char *typestr, int typestr_start, int typestr_end,
                                              va_list *ap, int *new_typestr_start);
static void free_adiak_type(adiak_datatype_t *t);
static adiak_type_t toplevel_type(const char *typestr);
static int copy_value(adiak_value_t *target, adiak_datatype_t *datatype, void *ptr);

adiak_datatype_t *adiak_new_datatype(const char *typestr, ...)
{
   va_list ap;
   adiak_datatype_t *t;
   va_start(ap, typestr);
   t = parse_typestr(typestr, &ap);
   va_end(ap);
   return t;
}

int adiak_raw_namevalue(const char *name, adiak_category_t category, adiak_value_t *value, adiak_datatype_t *type)
{
   adiak_tool_t *tool;
   for (tool = *tool_list; tool != NULL; tool = tool->next) {
      if (!tool->report_on_all_ranks && !global_adiak->reportable_rank)
         continue;
      if (tool->category != adiak_category_all && tool->category != category)
         continue;
      if (tool->name_val_cb)
         tool->name_val_cb(name, category, value, type, tool->opaque_val);
   }
   return 0;   
}

int adiak_namevalue(const char *name, adiak_category_t category, const char *typestr, ...)
{
   va_list ap;
   adiak_datatype_t *t;
   adiak_type_t toptype;
   adiak_value_t *value = NULL;
   void *container_ptr = NULL;

   toptype = toplevel_type(typestr);
   if (toptype == adiak_type_unset)
      return -1;
   value = (adiak_value_t *) malloc(sizeof(adiak_value_t));

   va_start(ap, typestr);

   switch (toptype) {
      case adiak_type_unset:
         return -1;
      case adiak_long:
      case adiak_ulong:
      case adiak_date:
         value->v_long = va_arg(ap, long);
         break;
      case adiak_int:
      case adiak_uint:
         value->v_int = va_arg(ap, int);
         break;
      case adiak_double:
         value->v_double = va_arg(ap, double);
         break;
      case adiak_timeval: {
         struct timeval *v = (struct timeval *) malloc(sizeof(struct timeval));
         *v = *va_arg(ap, struct timeval *);
         value->v_ptr = v;
         break;
      }
      case adiak_version:
      case adiak_string:
      case adiak_catstring:
      case adiak_path:
         value->v_ptr = strdup(va_arg(ap, void*));
         break;
      case adiak_range:
      case adiak_set:
      case adiak_list:
      case adiak_tuple:
         container_ptr = va_arg(ap, void*);
         break;
   }
   
   t = parse_typestr(typestr, &ap);
   va_end(ap);
   if (!t) {
      free(value);
      return -1;
   }

   if (container_ptr) {
      copy_value(value, t, container_ptr);
   }
   
   return adiak_raw_namevalue(name, category, value, t);
}

adiak_numerical_t adiak_numerical_from_type(adiak_type_t dtype)
{
   switch (dtype) {
      case adiak_type_unset:
         return adiak_numerical_unset;
      case adiak_long:
      case adiak_ulong:
      case adiak_int:
      case adiak_uint:
      case adiak_double:
         return adiak_rational;
      case adiak_date:
      case adiak_timeval:
         return adiak_interval;
      case adiak_version:
      case adiak_string:
         return adiak_ordinal;
      case adiak_catstring:
      case adiak_path:
      case adiak_range:
      case adiak_set:
      case adiak_list:
      case adiak_tuple:
         return adiak_categorical;
   }
   return adiak_numerical_unset;
}

void adiak_register_cb(int adiak_version, adiak_category_t category,
                       adiak_nameval_cb_t nv, int report_on_all_ranks, void *opaque_val)
{
   adiak_register(adiak_version, category, nv, report_on_all_ranks, opaque_val);
}

int adiak_walltime()
{
   measure_adiak_walltime = 1;
   return 0;
}

int adiak_systime()
{
   measure_adiak_systime = 1;
   return 0;   
}

int adiak_cputime()
{
   measure_adiak_cputime = 1;
   return 0;   
}

int adiak_job_size()
{
#if defined(MPI_VERSION)
   int size;
   if (!global_adiak->use_mpi)
      return -1;

   MPI_Comm_size(global_adiak->adiak_communicator, &size);
   return adiak_namevalue("jobsize", adiak_general, "%d", size);
#else
   return -1;
#endif
}

int adiak_mpitime()
{
#if defined(MPI_VERSION)
   if (!global_adiak->use_mpi)
      return -1;   
   return adiak_request(mpitime, 1);
#else
   return -1;
#endif
}

#if defined(MPI_VERSION)
void adiak_init(MPI_Comm *communicator)
{
   int rank;
   adiak_common_init();

   if (communicator) {
      global_adiak->adiak_communicator = *communicator;
      MPI_Comm_rank(global_adiak->adiak_communicator, &rank);
      global_adiak->reportable_rank = (rank == 0);
      global_adiak->use_mpi = 1;
   }
}
#else
void adiak_init(void *unused)
{
   adiak_common_init();
}
#endif

void adiak_fini()
{
   if (measure_adiak_cputime)
      adiak_measure_times(0, 1);
   if (measure_adiak_systime)
      adiak_measure_times(1, 0);
   if (measure_adiak_walltime)
      adiak_measure_walltime();
}

static void adiak_common_init()
{
   static int initialized = 0;
   if (initialized)
      return;
   initialized = 1;

   global_adiak = adiak_sys_init();
   assert(global_adiak);

   if (ADIAK_VERSION < global_adiak->minimum_version)
      global_adiak->minimum_version = ADIAK_VERSION;
   tool_list = global_adiak->tool_list;
}

static void adiak_register(int adiak_version, adiak_category_t category,
                           adiak_nameval_cb_t nv,
                           int report_on_all_ranks, void *opaque_val)
{
   adiak_tool_t *newtool;
   adiak_common_init();
   newtool = (adiak_tool_t *) malloc(sizeof(adiak_tool_t));
   memset(newtool, 0, sizeof(*newtool));
   newtool->version = adiak_version;
   newtool->opaque_val = opaque_val;
   newtool->report_on_all_ranks = report_on_all_ranks;
   newtool->name_val_cb = nv;
   newtool->category = category;
   newtool->next = *tool_list;
   newtool->prev = NULL;
   if (*tool_list) 
      (*tool_list)->prev = newtool;
   *tool_list = newtool;
   if (report_on_all_ranks && !global_adiak->report_on_all_ranks)
      global_adiak->report_on_all_ranks = 1;
}

static int find_end_brace(const char *typestr, char endchar, int typestr_start, int typestr_end) {
   int depth = 0;
   int cur = typestr_start;

   if (!typestr)
      return -1;

   while (cur < typestr_end) {
      if (typestr[cur] == '[' || typestr[cur] == '{' || typestr[cur] == '(' || typestr[cur] == '<')
         depth++;
      if (typestr[cur] == ']' || typestr[cur] == '}' || typestr[cur] == ')' || typestr[cur] == '>')
         depth--;
      if (depth == 0 && typestr[cur] == endchar)
         return cur;
      cur++;
   }
   return -1;      
}
static adiak_datatype_t *parse_typestr(const char *typestr, va_list *ap)
{
   int end = 0;
   int len;
   
   len = strlen(typestr);
   return parse_typestr_helper(typestr, 0, len, ap, &end);
}

static adiak_type_t toplevel_type(const char *typestr) {
   const char *cur = typestr;
   if (!cur)
      return adiak_type_unset;
   while (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == ',')
      cur++;
   if (*cur == '%') {
      cur++;      
      switch (*cur) {
         case 'l':
            cur++;
            if (*cur == 'd') return adiak_long;
            if (*cur == 'u') return adiak_ulong;
            return adiak_type_unset;
         case 'd': return adiak_int;
         case 'u': return adiak_uint;
         case 'f': return adiak_double;
         case 'D': return adiak_date;
         case 't': return adiak_timeval;
         case 'v': return adiak_version;
         case 's': return adiak_string;
         case 'r': return adiak_catstring;
         case 'p': return adiak_path;
         default:
            return adiak_type_unset;
      }
   }
   switch (*cur) {
      case '<': return adiak_range;
      case '[': return adiak_set;
      case '{': return adiak_list;
      case '(': return adiak_tuple;
      default: return adiak_type_unset;
   }
}

adiak_datatype_t *adiak_get_basetype(adiak_type_t t)
{
   switch (t) {
      case adiak_type_unset:
         return NULL;
      case adiak_long:
         return &base_long;
      case adiak_ulong:
         return &base_ulong;
      case adiak_int:
         return &base_int;
      case adiak_uint:
         return &base_uint;
      case adiak_double:
         return &base_double;
      case adiak_date:
         return &base_date;
      case adiak_timeval:
         return &base_timeval;
      case adiak_version:
         return &base_version;
      case adiak_string:
         return &base_string;
      case adiak_catstring:
         return &base_catstring;
      case adiak_path:
         return &base_path;
      case adiak_range:
      case adiak_set:
      case adiak_list:
      case adiak_tuple:
      default:
         return NULL;
   }
}

static void free_adiak_type(adiak_datatype_t *t)
{
   int i;
   if (t == NULL)
      return;
   if (adiak_get_basetype(t->dtype) == t)
      return;
   for (i = 0; i < t->num_subtypes; i++) {
      free_adiak_type(t->subtype[i]);
   }
   free(t);
}

static int copy_value(adiak_value_t *target, adiak_datatype_t *datatype, void *ptr) {
   int bytes_read = 0, result, type_index = 0, i;
   adiak_value_t *newvalues;
   switch (datatype->dtype) {
      case adiak_type_unset:
         return -1;
      case adiak_long:
      case adiak_ulong:
      case adiak_date:
         target->v_long = *((long *) ptr);
         return sizeof(long);
      case adiak_int:
      case adiak_uint:
         target->v_int = *((int *) ptr);
         return sizeof(int);
      case adiak_double:
         target->v_double= *((double *) ptr);
         return sizeof(double);
      case adiak_timeval: {
         struct timeval *v = (struct timeval *) malloc(sizeof(struct timeval));
         *v = *(struct timeval *) ptr;
         target->v_ptr = v;
         return sizeof(struct timeval *);
      }
      case adiak_version:
      case adiak_string:
      case adiak_catstring:
      case adiak_path:
         target->v_ptr = strdup(*((void **) ptr));
         return sizeof(char *);
      case adiak_range:
      case adiak_set:
      case adiak_list:
      case adiak_tuple:
         newvalues = (adiak_value_t *) malloc(sizeof(adiak_value_t) * datatype->num_elements);
         for (i = 0; i < datatype->num_elements; i++) {
            unsigned char *array_base = (unsigned char *) ptr;
            result = copy_value(newvalues+i, datatype->subtype[type_index], array_base + bytes_read);
            if (result == -1)
               return -1;
            bytes_read += result;
            if (datatype->dtype == adiak_tuple)
               type_index++;
         }
         target->v_ptr = newvalues;
         return sizeof(void*);
   }
   return -1;
}

static adiak_datatype_t *parse_typestr_helper(const char *typestr, int typestr_start, int typestr_end,
                                       va_list *ap, int *new_typestr_start)

{
   adiak_datatype_t *t = NULL;
   int cur = typestr_start;
   int end_brace, i, is_long = 0;
   
   if (!typestr)
      goto error;
   if (typestr_start == typestr_end)
      goto error;

   while (typestr[cur] == ' ' || typestr[cur] == '\t' || typestr[cur] == '\n' || typestr[cur] == ',')
      cur++;

   if (strchr("[{<(", typestr[cur])) {
      t = (adiak_datatype_t *) malloc(sizeof(adiak_datatype_t));
      memset(t, 0, sizeof(*t));
   }
   if (typestr[cur] == '{' || typestr[cur] == '[') {
      end_brace = find_end_brace(typestr, typestr[cur] == '{' ? '}' : ']',
                                 cur, typestr_end);
      if (end_brace == -1)
         goto error;
      t->num_elements = va_arg(*ap, int);
      t->dtype = typestr[cur] == '{' ? adiak_list : adiak_set;
      t->numerical = adiak_categorical;
      t->num_subtypes = 1;
      t->subtype = (adiak_datatype_t **) malloc(sizeof(adiak_datatype_t *));
      t->subtype[0] = parse_typestr_helper(typestr, cur+1, end_brace, ap, &cur);
      if (t->subtype[0] == NULL)
         goto error;
      *new_typestr_start = end_brace+1;
      goto done;
   }
   else if (typestr[cur] == '<') {
      end_brace = find_end_brace(typestr, '>', cur, typestr_end);
      if (end_brace == -1)
         goto error;
      t->dtype = adiak_range;
      t->num_elements = 2;
      t->numerical = adiak_categorical;
      t->num_subtypes = 1;
      t->subtype = (adiak_datatype_t **) malloc(sizeof(adiak_datatype_t *));
      t->subtype[0] = parse_typestr_helper(typestr, cur+1, end_brace, ap, &cur);
      if (t->subtype[0] == NULL)
         goto error;
      *new_typestr_start = end_brace+1;      
      goto done;
   }
   else if (typestr[cur] == '(') {
      end_brace = find_end_brace(typestr, ')', cur, typestr_end);
      if (end_brace == -1)
         goto error;
      t->dtype = adiak_tuple;
      t->numerical = adiak_categorical;
      t->num_subtypes = t->num_elements = va_arg(*ap, int);
      t->subtype = (adiak_datatype_t **) malloc(sizeof(adiak_datatype_t *) * t->num_subtypes);
      memset(t->subtype, 0, sizeof(adiak_datatype_t *) * t->num_subtypes);
      cur++;
      for (i = 0; i < t->num_subtypes; i++) {
         t->subtype[i] = parse_typestr_helper(typestr, cur, end_brace, ap, &cur);
         if (!t->subtype[i])
            goto error;
      }
   }
   else if (typestr[cur] == '%') {
      cur++;
      if (typestr[cur] == 'l') {
         is_long = 1;
         cur++;
      }
      switch (typestr[cur]) {
         case 'd':
            t = is_long ? &base_long : &base_int;
            break;
         case 'u':
            t = is_long ? &base_ulong : &base_uint;
            break;
         case 'f':
            t = &base_double;
            break;
         case 'D':
            t = &base_date;
            break;
         case 't':
            t = &base_timeval;
            break;
         case 'v':
            t = &base_version;
            break;
         case 's':
            t = &base_string;
            break;
         case 'r':
            t = &base_catstring;
            break;
         case 'p':
            t = &base_path;
            break;
         default:
            goto error;
      }
      cur++;
      *new_typestr_start = cur;
      goto done;
   }

  done:
   return t;
  error:
   if (t)
      free_adiak_type(t);
   return NULL;
}
