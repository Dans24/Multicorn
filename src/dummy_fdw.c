/*-------------------------------------------------------------------------
 *
 *          foreign-data wrapper for dummy
 *
 * copyright (c) 2011, postgresql global development group
 *
 * this software is released under the postgresql licence
 *
 * author: dickson s. guedes <guedes@guedesoft.net>
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include <Python.h>

PG_MODULE_MAGIC;

/*
 * Options for the dummy wrapper
 */

typedef struct DummyState
{
  AttInMetadata *attinmeta;
  int rownum;
  PyObject *pFunc;
  PyObject *pIterator;
} DummyState;

extern Datum dummy_fdw_handler(PG_FUNCTION_ARGS);
extern Datum dummy_fdw_validator(PG_FUNCTION_ARGS);


PG_FUNCTION_INFO_V1(dummy_fdw_handler);
PG_FUNCTION_INFO_V1(dummy_fdw_validator);

/*
 * FDW functions declarations
 */
static FdwPlan *dummy_plan(Oid foreign_table_id, PlannerInfo *root, RelOptInfo *base_relation);
static void dummy_explain(ForeignScanState *node, ExplainState *es);
static void dummy_begin(ForeignScanState *node, int eflags);
static TupleTableSlot *dummy_iterate(ForeignScanState *node);
static void dummy_rescan(ForeignScanState *node);
static void dummy_end(ForeignScanState *node);

/*
  Helpers
*/
static void dummy_get_options(Oid foreign_table_id, PyObject *options_dict, char **module);
static HeapTuple pydict_to_postgres_tuple(TupleDesc desc, PyObject *pydict);
static char* pyobject_to_cstring(PyObject *pyobject);

const char* DATE_FORMAT_STRING = "%Y-%m-%d";

Datum
dummy_fdw_handler(PG_FUNCTION_ARGS)
{
  FdwRoutine *fdw_routine = makeNode(FdwRoutine);

  fdw_routine->PlanForeignScan = dummy_plan;
  fdw_routine->ExplainForeignScan = dummy_explain;
  fdw_routine->BeginForeignScan = dummy_begin;
  fdw_routine->IterateForeignScan = dummy_iterate;
  fdw_routine->ReScanForeignScan = dummy_rescan;
  fdw_routine->EndForeignScan = dummy_end;

  PG_RETURN_POINTER(fdw_routine);
}

Datum
dummy_fdw_validator(PG_FUNCTION_ARGS)
{
  PG_RETURN_BOOL(true);
}

static FdwPlan *
dummy_plan( Oid foreign_table_id,
            PlannerInfo *root,
            RelOptInfo  *base_relation)
{
  FdwPlan *fdw_plan;

  fdw_plan = makeNode(FdwPlan);

  fdw_plan->startup_cost = 10;
  base_relation->rows = 1;
  fdw_plan->total_cost = 15;

  return fdw_plan;
}

static void
dummy_explain(ForeignScanState *node, ExplainState *es)
{
  /* TODO: calculate real values */
  ExplainPropertyText("Foreign dummy", "dummy", es);

  if (es->costs)
    {
      ExplainPropertyLong("Foreign dummy cost", 10.5, es);
    }
}


static void
dummy_begin(ForeignScanState *node, int eflags)
{
  /*  TODO: do things if necessary */
  AttInMetadata  *attinmeta;
  Relation        rel = node->ss.ss_currentRelation;
  DummyState      *state;
  PyObject *pName, *pModule, *pArgs, *pValue, *options_dict, *pIterator, *pFunc, *pClass, *pObj, *pMethod;
  char *module;

  attinmeta = TupleDescGetAttInMetadata(rel->rd_att);
  state = (DummyState *) palloc(sizeof(DummyState));
  state->rownum = 0;
  state->attinmeta = attinmeta;
  node->fdw_state = (void *) state;

  Py_Initialize();
  elog(INFO, "Getting options");
  options_dict = PyDict_New();
  dummy_get_options(RelationGetRelid(node->ss.ss_currentRelation),
                    options_dict, &module);

  elog(INFO, "Getting Root Module fdw");
  pName = PyUnicode_FromString("fdw");
  pModule = PyImport_Import(pName);
  Py_DECREF(pName);

  if (pModule != NULL) {
    elog(INFO, "Getting Module %s", module);
    pArgs = PyTuple_New(1);
    PyTuple_SetItem(pArgs, 0, PyString_FromString(module));
    pFunc = PyObject_GetAttrString(pModule, "getClass");
    elog(INFO, "Calling getClass");
    pClass = PyObject_CallObject(pFunc, pArgs);
    Py_DECREF(pArgs);
    Py_DECREF(pFunc);

    elog(INFO, "Prepare Class __init__");

    pArgs = PyTuple_New(1);
    elog(INFO, "Setting dict");
    PyTuple_SetItem(pArgs, 0, options_dict);
    elog(INFO, "Instantiating class");
    pObj = PyObject_CallObject(pClass, pArgs);
    elog(INFO, "Object created %d", pObj);
    Py_DECREF(pArgs);
    pArgs = PyTuple_New(0);
    /* PyTuple_SetItem(pArgs, 0, pObj); */
    pMethod = PyObject_GetAttrString(pObj, "execute");
    pValue = PyObject_CallObject(pMethod, pArgs);

    state->pIterator = PyObject_GetIter(pValue);

    Py_DECREF(pValue);
    Py_DECREF(pArgs);
    Py_DECREF(pModule);
    /* if (!(state->pFunc && PyCallable_Check(state->pFunc))) { */
      /* if (PyErr_Occurred()) */
        /* PyErr_Print(); */
      /* elog(ERROR, "Cannot find function 'get'"); */
    /* } */
  }
  else {
    PyErr_Print();
    elog(ERROR, "Failed to load module");
  }
  elog(INFO, "End begin");
}


static TupleTableSlot *
dummy_iterate(ForeignScanState *node)
{
  TupleTableSlot            *slot = node->ss.ss_ScanTupleSlot;
  Relation                relation = node->ss.ss_currentRelation;

  DummyState      *state = (DummyState *) node->fdw_state;

  HeapTuple        tuple;

  int                total_attributes, i;
  char            **tup_values;
  MemoryContext        oldcontext;
  PyObject *pValue, *pArgs, *pIterator;

  elog(INFO, "Iterate");
  ExecClearTuple(slot);
  total_attributes = relation->rd_att->natts;
  tup_values = (char **) palloc(sizeof(char *) * total_attributes);

  if(state->rownum > 10){
    return slot;
  }
  pArgs = PyTuple_New(0);
  pIterator = state->pIterator;
  Py_DECREF(pArgs);
  if (pValue != NULL) {
    Py_DECREF(pValue);
  }
  if (pIterator == NULL) {
      /* propagate error */
  }
  pValue = PyIter_Next(pIterator);
  if (PyErr_Occurred()) {
    /* Stop iteration */
    PyErr_Print();
    return slot;
  }
  if (pValue == NULL){
    return slot;
  }

  oldcontext = MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_query_memory);
  MemoryContextSwitchTo(oldcontext);
  tuple = pydict_to_postgres_tuple(node->ss.ss_currentRelation->rd_att
          , pValue);
  ExecStoreTuple(tuple, slot, InvalidBuffer, false);
  elog(INFO, "Returning slot");
  Py_DECREF(pValue);
  state->rownum++;
  return slot;
}

static void
dummy_rescan(ForeignScanState *node)
{
  DummyState *state = (DummyState *) node->fdw_state;
  state->rownum = 0;
}

static void
dummy_end(ForeignScanState *node)
{
  DummyState *state = (DummyState *) node->fdw_state;
  Py_DECREF(state->pIterator);
  Py_Finalize();
}


static void
dummy_get_options(Oid foreign_table_id, PyObject *options_dict, char **module)
{
  ForeignTable    *f_table;
  ForeignServer    *f_server;
  List            *options;
  ListCell        *lc;
  bool got_module = false;
  f_table = GetForeignTable(foreign_table_id);
  f_server = GetForeignServer(f_table->serverid);

  options = NIL;
  options = list_concat(options, f_table->options);
  options = list_concat(options, f_server->options);

  foreach(lc, options) {

    DefElem *def = (DefElem *) lfirst(lc);

    if (strcmp(def->defname, "wrapper") == 0) {
      *module = defGetString(def);
      got_module = true;
    } else {
      PyDict_SetItemString(options_dict, def->defname,
                           PyString_FromString(defGetString(def)));
    }
  }
  if (!got_module) {
    ereport(ERROR,
            (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
             errmsg("wrapper option not found"),
             errhint("You must set wrapper option to a ForeignDataWrapper python class, for example fdw.csv.CsvFdw")));
  }
}

static HeapTuple
pydict_to_postgres_tuple(TupleDesc desc, PyObject *pydict)
{
  HeapTuple tuple;
  PyObject *items = PyMapping_Items(pydict);
  AttInMetadata *attinmeta = TupleDescGetAttInMetadata(desc);
  char * current_value, key;
  char **tup_values;
  int i, natts;
  natts = desc->natts;
  tup_values = (char **) palloc(sizeof(char *) * natts);
  for(i = 0; i< natts; i++){
    key = NameStr(desc->attrs[i]->attname);
    tup_values[i] = pyobject_to_cstring(PyMapping_GetItemString(pydict, key));
  }
  tuple = BuildTupleFromCStrings(attinmeta, tup_values);
  return tuple;
}

static char* pyobject_to_cstring(PyObject *pyobject)
{
    PyObject * date_module = PyImport_Import(
                PyUnicode_FromString("datetime"));
    PyObject * date_cls = PyObject_GetAttrString(date_module, "date");
    if(PyNumber_Check(pyobject)){
        return PyString_AsString(PyObject_Str(pyobject));
    }
    if(PyObject_IsInstance(pyobject, date_cls)){
        PyObject * date_format_method = PyObject_GetAttrString(pyobject, "strftime");
        PyObject * pArgs = PyTuple_New(1);
        PyTuple_SetItem(pArgs, 0, PyString_FromString(DATE_FORMAT_STRING));
        PyObject * formatted_date = PyObject_CallObject(date_format_method, pArgs);
        char * result = PyString_AsString(formatted_date);
        return PyString_AsString(formatted_date);
    }
    return PyString_AsString(pyobject);
}
