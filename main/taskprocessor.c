/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007-2008, Digium, Inc.
 *
 * Dwayne M. Hubbard <dhubbard@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \brief Maintain a container of uniquely-named taskprocessor threads that can be shared across modules.
 *
 * \author Dwayne Hubbard <dhubbard@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/_private.h"
#include "asterisk/module.h"
#include "asterisk/time.h"
#include "asterisk/astobj2.h"
#include "asterisk/cli.h"
#include "asterisk/taskprocessor.h"


/*!
 * \brief tps_task structure is queued to a taskprocessor
 *
 * tps_tasks are processed in FIFO order and freed by the taskprocessing
 * thread after the task handler returns.  The callback function that is assigned
 * to the execute() function pointer is responsible for releasing datap resources if necessary.
 */
struct tps_task {
	/*! \brief The execute() task callback function pointer */
	int (*execute)(void *datap);
	/*! \brief The data pointer for the task execute() function */
	void *datap;
	/*! \brief AST_LIST_ENTRY overhead */
	AST_LIST_ENTRY(tps_task) list;
};

/*! \brief tps_taskprocessor_stats maintain statistics for a taskprocessor. */
struct tps_taskprocessor_stats {
	/*! \brief This is the maximum number of tasks queued at any one time */
	unsigned long max_qsize;
	/*! \brief This is the current number of tasks processed */
	unsigned long _tasks_processed_count;
};

/*! \brief A ast_taskprocessor structure is a singleton by name */
struct ast_taskprocessor {
	/*! \brief Friendly name of the taskprocessor */
	const char *name;
	/*! \brief Taskprocessor statistics */
	struct tps_taskprocessor_stats *stats;
	/*! \brief Taskprocessor current queue size */
	long tps_queue_size;
	/*! \brief Taskprocessor queue */
	AST_LIST_HEAD_NOLOCK(tps_queue, tps_task) tps_queue;
	/*! \brief Taskprocessor singleton list entry */
	AST_LIST_ENTRY(ast_taskprocessor) list;
	struct ast_taskprocessor_listener *listener;
	/*! Indicates if the taskprocessor is in the process of shuting down */
	unsigned int shutting_down:1;
};
#define TPS_MAX_BUCKETS 7
/*! \brief tps_singletons is the astobj2 container for taskprocessor singletons */
static struct ao2_container *tps_singletons;

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> operation requires a ping condition */
static ast_cond_t cli_ping_cond;

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> operation requires a ping condition lock */
AST_MUTEX_DEFINE_STATIC(cli_ping_cond_lock);

/*! \brief The astobj2 hash callback for taskprocessors */
static int tps_hash_cb(const void *obj, const int flags);
/*! \brief The astobj2 compare callback for taskprocessors */
static int tps_cmp_cb(void *obj, void *arg, int flags);

/*! \brief The task processing function executed by a taskprocessor */
static void *tps_processing_function(void *data);

/*! \brief Destroy the taskprocessor when its refcount reaches zero */
static void tps_taskprocessor_destroy(void *tps);

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> handler function */
static int tps_ping_handler(void *datap);

/*! \brief Remove the front task off the taskprocessor queue */
static struct tps_task *tps_taskprocessor_pop(struct ast_taskprocessor *tps);

/*! \brief Return the size of the taskprocessor queue */
static int tps_taskprocessor_depth(struct ast_taskprocessor *tps);

static char *cli_tps_ping(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static char *cli_tps_report(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);

static struct ast_cli_entry taskprocessor_clis[] = {
	AST_CLI_DEFINE(cli_tps_ping, "Ping a named task processor"),
	AST_CLI_DEFINE(cli_tps_report, "List instantiated task processors and statistics"),
};

struct default_taskprocessor_listener_pvt {
	pthread_t poll_thread;
	ast_mutex_t lock;
	ast_cond_t cond;
	int wake_up;
	int dead;
};


static void default_tps_wake_up(struct default_taskprocessor_listener_pvt *pvt, int should_die)
{
	SCOPED_MUTEX(lock, &pvt->lock);
	pvt->wake_up = 1;
	pvt->dead = should_die;
	ast_cond_signal(&pvt->cond);
}

static int default_tps_idle(struct default_taskprocessor_listener_pvt *pvt)
{
	SCOPED_MUTEX(lock, &pvt->lock);
	while (!pvt->wake_up) {
		ast_cond_wait(&pvt->cond, lock);
	}
	pvt->wake_up = 0;
	return pvt->dead;
}

/* this is the task processing worker function */
static void *tps_processing_function(void *data)
{
	struct ast_taskprocessor_listener *listener = data;
	struct ast_taskprocessor *tps = listener->tps;
	struct default_taskprocessor_listener_pvt *pvt = listener->private_data;
	int dead = 0;

	while (!dead) {
		if (!ast_taskprocessor_execute(tps)) {
			dead = default_tps_idle(pvt);
		}
	}
	return NULL;
}

static void *default_listener_alloc(struct ast_taskprocessor_listener *listener)
{
	struct default_taskprocessor_listener_pvt *pvt;

	pvt = ast_calloc(1, sizeof(*pvt));
	if (!pvt) {
		return NULL;
	}
	ast_cond_init(&pvt->cond, NULL);
	ast_mutex_init(&pvt->lock);
	pvt->poll_thread = AST_PTHREADT_NULL;
	return pvt;
}

static int default_listener_start(struct ast_taskprocessor_listener *listener)
{
	struct default_taskprocessor_listener_pvt *pvt = listener->private_data;

	if (ast_pthread_create(&pvt->poll_thread, NULL, tps_processing_function, listener)) {
		return -1;
	}

	return 0;
}

static void default_task_pushed(struct ast_taskprocessor_listener *listener, int was_empty)
{
	struct default_taskprocessor_listener_pvt *pvt = listener->private_data;

	if (was_empty) {
		default_tps_wake_up(pvt, 0);
	}
}

static void default_emptied(struct ast_taskprocessor_listener *listener)
{
	/* No-op */
}

static void default_listener_shutdown(struct ast_taskprocessor_listener *listener)
{
	struct default_taskprocessor_listener_pvt *pvt = listener->private_data;
	default_tps_wake_up(pvt, 1);
	pthread_join(pvt->poll_thread, NULL);
	pvt->poll_thread = AST_PTHREADT_NULL;
}

static void default_listener_destroy(void *obj)
{
	struct default_taskprocessor_listener_pvt *pvt = obj;
	ast_mutex_destroy(&pvt->lock);
	ast_cond_destroy(&pvt->cond);
	ast_free(pvt);
}

static const struct ast_taskprocessor_listener_callbacks default_listener_callbacks = {
	.alloc = default_listener_alloc,
	.start = default_listener_start,
	.task_pushed = default_task_pushed,
	.emptied = default_emptied,
	.shutdown = default_listener_shutdown,
	.destroy = default_listener_destroy,
};

/*! \internal \brief Clean up resources on Asterisk shutdown */
static void tps_shutdown(void)
{
	ao2_t_ref(tps_singletons, -1, "Unref tps_singletons in shutdown");
}

/* initialize the taskprocessor container and register CLI operations */
int ast_tps_init(void)
{
	if (!(tps_singletons = ao2_container_alloc(TPS_MAX_BUCKETS, tps_hash_cb, tps_cmp_cb))) {
		ast_log(LOG_ERROR, "taskprocessor container failed to initialize!\n");
		return -1;
	}

	ast_cond_init(&cli_ping_cond, NULL);

	ast_cli_register_multiple(taskprocessor_clis, ARRAY_LEN(taskprocessor_clis));

	ast_register_atexit(tps_shutdown);

	return 0;
}

/* allocate resources for the task */
static struct tps_task *tps_task_alloc(int (*task_exe)(void *datap), void *datap)
{
	struct tps_task *t;
	if ((t = ast_calloc(1, sizeof(*t)))) {
		t->execute = task_exe;
		t->datap = datap;
	}
	return t;
}

/* release task resources */
static void *tps_task_free(struct tps_task *task)
{
	if (task) {
		ast_free(task);
	}
	return NULL;
}

/* taskprocessor tab completion */
static char *tps_taskprocessor_tab_complete(struct ast_taskprocessor *p, struct ast_cli_args *a)
{
	int tklen;
	int wordnum = 0;
	char *name = NULL;
	struct ao2_iterator i;

	if (a->pos != 3)
		return NULL;

	tklen = strlen(a->word);
	i = ao2_iterator_init(tps_singletons, 0);
	while ((p = ao2_iterator_next(&i))) {
		if (!strncasecmp(a->word, p->name, tklen) && ++wordnum > a->n) {
			name = ast_strdup(p->name);
			ao2_ref(p, -1);
			break;
		}
		ao2_ref(p, -1);
	}
	ao2_iterator_destroy(&i);
	return name;
}

/* ping task handling function */
static int tps_ping_handler(void *datap)
{
	ast_mutex_lock(&cli_ping_cond_lock);
	ast_cond_signal(&cli_ping_cond);
	ast_mutex_unlock(&cli_ping_cond_lock);
	return 0;
}

/* ping the specified taskprocessor and display the ping time on the CLI */
static char *cli_tps_ping(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct timeval begin, end, delta;
	const char *name;
	struct timeval when;
	struct timespec ts;
	struct ast_taskprocessor *tps = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core ping taskprocessor";
		e->usage =
			"Usage: core ping taskprocessor <taskprocessor>\n"
			"	Displays the time required for a task to be processed\n";
		return NULL;
	case CLI_GENERATE:
		return tps_taskprocessor_tab_complete(tps, a);
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	name = a->argv[3];
	if (!(tps = ast_taskprocessor_get(name, TPS_REF_IF_EXISTS))) {
		ast_cli(a->fd, "\nping failed: %s not found\n\n", name);
		return CLI_SUCCESS;
	}
	ast_cli(a->fd, "\npinging %s ...", name);
	when = ast_tvadd((begin = ast_tvnow()), ast_samp2tv(1000, 1000));
	ts.tv_sec = when.tv_sec;
	ts.tv_nsec = when.tv_usec * 1000;
	ast_mutex_lock(&cli_ping_cond_lock);
	if (ast_taskprocessor_push(tps, tps_ping_handler, 0) < 0) {
		ast_cli(a->fd, "\nping failed: could not push task to %s\n\n", name);
		ao2_ref(tps, -1);
		return CLI_FAILURE;
	}
	ast_cond_timedwait(&cli_ping_cond, &cli_ping_cond_lock, &ts);
	ast_mutex_unlock(&cli_ping_cond_lock);
	end = ast_tvnow();
	delta = ast_tvsub(end, begin);
	ast_cli(a->fd, "\n\t%24s ping time: %.1ld.%.6ld sec\n\n", name, (long)delta.tv_sec, (long int)delta.tv_usec);
	ao2_ref(tps, -1);
	return CLI_SUCCESS;
}

static char *cli_tps_report(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char name[256];
	int tcount;
	unsigned long qsize;
	unsigned long maxqsize;
	unsigned long processed;
	struct ast_taskprocessor *p;
	struct ao2_iterator i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show taskprocessors";
		e->usage =
			"Usage: core show taskprocessors\n"
			"	Shows a list of instantiated task processors and their statistics\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, "\n\t+----- Processor -----+--- Processed ---+- In Queue -+- Max Depth -+");
	i = ao2_iterator_init(tps_singletons, 0);
	while ((p = ao2_iterator_next(&i))) {
		ast_copy_string(name, p->name, sizeof(name));
		qsize = p->tps_queue_size;
		maxqsize = p->stats->max_qsize;
		processed = p->stats->_tasks_processed_count;
		ast_cli(a->fd, "\n%24s   %17ld %12ld %12ld", name, processed, qsize, maxqsize);
		ao2_ref(p, -1);
	}
	ao2_iterator_destroy(&i);
	tcount = ao2_container_count(tps_singletons);
	ast_cli(a->fd, "\n\t+---------------------+-----------------+------------+-------------+\n\t%d taskprocessors\n\n", tcount);
	return CLI_SUCCESS;
}

/* hash callback for astobj2 */
static int tps_hash_cb(const void *obj, const int flags)
{
	const struct ast_taskprocessor *tps = obj;
	const char *name = flags & OBJ_KEY ? obj : tps->name;

	return ast_str_case_hash(name);
}

/* compare callback for astobj2 */
static int tps_cmp_cb(void *obj, void *arg, int flags)
{
	struct ast_taskprocessor *lhs = obj, *rhs = arg;
	const char *rhsname = flags & OBJ_KEY ? arg : rhs->name;

	return !strcasecmp(lhs->name, rhsname) ? CMP_MATCH | CMP_STOP : 0;
}

/* destroy the taskprocessor */
static void tps_taskprocessor_destroy(void *tps)
{
	struct ast_taskprocessor *t = tps;

	if (!tps) {
		ast_log(LOG_ERROR, "missing taskprocessor\n");
		return;
	}
	ast_debug(1, "destroying taskprocessor '%s'\n", t->name);
	/* free it */
	if (t->stats) {
		ast_free(t->stats);
		t->stats = NULL;
	}
	ast_free((char *) t->name);
	if (t->listener) {
		/* This code should not be reached since the listener
		 * should have been destroyed before the taskprocessor could
		 * be destroyed
		 */
		ao2_ref(t->listener, -1);
		t->listener = NULL;
	}
}

/* pop the front task and return it */
static struct tps_task *tps_taskprocessor_pop(struct ast_taskprocessor *tps)
{
	struct tps_task *task;
	SCOPED_AO2LOCK(lock, tps);

	if (tps->shutting_down) {
		return NULL;
	}

	if ((task = AST_LIST_REMOVE_HEAD(&tps->tps_queue, list))) {
		tps->tps_queue_size--;
	}
	return task;
}

static int tps_taskprocessor_depth(struct ast_taskprocessor *tps)
{
	return (tps) ? tps->tps_queue_size : -1;
}

/* taskprocessor name accessor */
const char *ast_taskprocessor_name(struct ast_taskprocessor *tps)
{
	if (!tps) {
		ast_log(LOG_ERROR, "no taskprocessor specified!\n");
		return NULL;
	}
	return tps->name;
}

static void listener_destroy(void *obj)
{
	struct ast_taskprocessor_listener *listener = obj;

	listener->callbacks->destroy(listener->private_data);
}

static void listener_shutdown(struct ast_taskprocessor_listener *listener)
{
	listener->callbacks->shutdown(listener);
	ao2_ref(listener->tps, -1);
	listener->tps = NULL;
}

struct ast_taskprocessor_listener *ast_taskprocessor_listener_alloc(const struct ast_taskprocessor_listener_callbacks *callbacks)
{
	RAII_VAR(struct ast_taskprocessor_listener *, listener,
			ao2_alloc(sizeof(*listener), listener_destroy), ao2_cleanup);

	if (!listener) {
		return NULL;
	}
	listener->callbacks = callbacks;
	listener->private_data = listener->callbacks->alloc(listener);
	if (!listener->private_data) {
		return NULL;
	}

	ao2_ref(listener, +1);
	return listener;
}

/* Provide a reference to a taskprocessor.  Create the taskprocessor if necessary, but don't
 * create the taskprocessor if we were told via ast_tps_options to return a reference only
 * if it already exists */
struct ast_taskprocessor *ast_taskprocessor_get(const char *name, enum ast_tps_options create)
{
	struct ast_taskprocessor *p;
	struct ast_taskprocessor_listener *listener;

	if (ast_strlen_zero(name)) {
		ast_log(LOG_ERROR, "requesting a nameless taskprocessor!!!\n");
		return NULL;
	}
	p = ao2_find(tps_singletons, name, OBJ_KEY);
	if (p) {
		ao2_unlock(tps_singletons);
		return p;
	}
	if (create & TPS_REF_IF_EXISTS) {
		/* calling function does not want a new taskprocessor to be created if it doesn't already exist */
		return NULL;
	}
	/* Create a new taskprocessor. Start by creating a default listener */
	listener = ast_taskprocessor_listener_alloc(&default_listener_callbacks);
	if (!listener) {
		return NULL;
	}

	p = ast_taskprocessor_create_with_listener(name, listener);
	if (!p) {
		ao2_ref(listener, -1);
		return NULL;
	}

	/* Unref listener here since the taskprocessor has gained a reference to the listener */
	ao2_ref(listener, -1);
	return p;

}

struct ast_taskprocessor *ast_taskprocessor_create_with_listener(const char *name, struct ast_taskprocessor_listener *listener)
{
	RAII_VAR(struct ast_taskprocessor *, p,
			ao2_alloc(sizeof(*p), tps_taskprocessor_destroy),
			ao2_cleanup);

	if (!p) {
		ast_log(LOG_WARNING, "failed to create taskprocessor '%s'\n", name);
		return NULL;
	}

	if (!(p->stats = ast_calloc(1, sizeof(*p->stats)))) {
		ast_log(LOG_WARNING, "failed to create taskprocessor stats for '%s'\n", name);
		return NULL;
	}
	if (!(p->name = ast_strdup(name))) {
		ao2_ref(p, -1);
		return NULL;
	}

	ao2_ref(listener, +1);
	p->listener = listener;

	ao2_ref(p, +1);
	listener->tps = p;

	if (!(ao2_link(tps_singletons, p))) {
		ast_log(LOG_ERROR, "Failed to add taskprocessor '%s' to container\n", p->name);
		return NULL;
	}

	if (p->listener->callbacks->start(p->listener)) {
		ast_log(LOG_ERROR, "Unable to start taskprocessor listener for taskprocessor %s\n", p->name);
		ast_taskprocessor_unreference(p);
		return NULL;
	}

	/* RAII_VAR will decrement the refcount at the end of the function.
	 * Since we want to pass back a reference to p, we bump the refcount
	 */
	ao2_ref(p, +1);
	return p;
}

/* decrement the taskprocessor reference count and unlink from the container if necessary */
void *ast_taskprocessor_unreference(struct ast_taskprocessor *tps)
{
	struct ast_taskprocessor_listener *listener;

	if (!tps) {
		return NULL;
	}

	if (ao2_ref(tps, -1) > 3) {
		return NULL;
	}
	/* If we're down to 3 references, then those must be:
	 * 1. The reference we just got rid of
	 * 2. The container
	 * 3. The listener
	 */
	ao2_unlink(tps_singletons, tps);
	listener = tps->listener;
	tps->listener = NULL;
	listener_shutdown(listener);
	ao2_ref(listener, -1);
	return NULL;
}

/* push the task into the taskprocessor queue */
int ast_taskprocessor_push(struct ast_taskprocessor *tps, int (*task_exe)(void *datap), void *datap)
{
	struct tps_task *t;
	int previous_size;

	if (!tps || !task_exe) {
		ast_log(LOG_ERROR, "%s is missing!!\n", (tps) ? "task callback" : "taskprocessor");
		return -1;
	}
	if (!(t = tps_task_alloc(task_exe, datap))) {
		ast_log(LOG_ERROR, "failed to allocate task!  Can't push to '%s'\n", tps->name);
		return -1;
	}
	ao2_lock(tps);
	AST_LIST_INSERT_TAIL(&tps->tps_queue, t, list);
	previous_size = tps->tps_queue_size++;
	ao2_unlock(tps);
	tps->listener->callbacks->task_pushed(tps->listener, previous_size ? 0 : 1);
	return 0;
}

int ast_taskprocessor_execute(struct ast_taskprocessor *tps)
{
	struct tps_task *t;
	int size;

	if (!(t = tps_taskprocessor_pop(tps))) {
		return 0;
	}

	t->execute(t->datap);

	tps_task_free(t);

	ao2_lock(tps);
	size = tps_taskprocessor_depth(tps);
	if (tps->stats) {
		tps->stats->_tasks_processed_count++;
		if (size > tps->stats->max_qsize) {
			tps->stats->max_qsize = size;
		}
	}
	ao2_unlock(tps);

	if (size == 0) {
		tps->listener->callbacks->emptied(tps->listener);
		return 0;
	}
	return 1;
}
