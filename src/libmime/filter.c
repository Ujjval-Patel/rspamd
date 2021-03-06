/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "mem_pool.h"
#include "filter.h"
#include "rspamd.h"
#include "message.h"
#include "lua/lua_common.h"
#include <math.h>
#include "contrib/uthash/utlist.h"

/* Average symbols count to optimize hash allocation */
static struct rspamd_counter_data symbols_count;

static void
rspamd_metric_result_dtor (gpointer d)
{
	struct rspamd_metric_result *r = (struct rspamd_metric_result *)d;
	struct rspamd_symbol_result sres;

	rspamd_set_counter_ema (&symbols_count, kh_size (r->symbols), 0.5);

	kh_foreach_value (r->symbols, sres, {
		if (sres.options) {
			kh_destroy (rspamd_options_hash, sres.options);
		}
	});
	kh_destroy (rspamd_symbols_hash, r->symbols);
	kh_destroy (rspamd_symbols_group_hash, r->sym_groups);
}

struct rspamd_metric_result *
rspamd_create_metric_result (struct rspamd_task *task)
{
	struct rspamd_metric_result *metric_res;
	guint i;

	metric_res = task->result;

	if (metric_res != NULL) {
		return metric_res;
	}

	metric_res = rspamd_mempool_alloc0 (task->task_pool,
			sizeof (struct rspamd_metric_result));
	metric_res->symbols = kh_init (rspamd_symbols_hash);
	metric_res->sym_groups = kh_init (rspamd_symbols_group_hash);

	/* Optimize allocation */
	kh_resize (rspamd_symbols_group_hash, metric_res->sym_groups, 4);

	if (symbols_count.mean > 4) {
		kh_resize (rspamd_symbols_hash, metric_res->symbols, symbols_count.mean);
	}
	else {
		kh_resize (rspamd_symbols_hash, metric_res->symbols, 4);
	}

	if (task->cfg) {
		for (i = 0; i < METRIC_ACTION_MAX; i++) {
			metric_res->actions_limits[i] = task->cfg->actions[i].score;
		}
	}
	else {
		for (i = 0; i < METRIC_ACTION_MAX; i++) {
			metric_res->actions_limits[i] = NAN;
		}
	}

	rspamd_mempool_add_destructor (task->task_pool,
			rspamd_metric_result_dtor,
			metric_res);

	return metric_res;
}

static inline int
rspamd_pr_sort (const struct rspamd_passthrough_result *pra,
				const struct rspamd_passthrough_result *prb)
{
	return prb->priority - pra->priority;
}

void
rspamd_add_passthrough_result (struct rspamd_task *task,
									enum rspamd_action_type action,
									guint priority,
									double target_score,
									const gchar *message,
									const gchar *module)
{
	struct rspamd_metric_result *metric_res;
	struct rspamd_passthrough_result *pr;

	metric_res = task->result;

	pr = rspamd_mempool_alloc (task->task_pool, sizeof (*pr));
	pr->action = action;
	pr->priority = priority;
	pr->message = message;
	pr->module = module;
	pr->target_score = target_score;

	DL_APPEND (metric_res->passthrough_result, pr);
	DL_SORT (metric_res->passthrough_result, rspamd_pr_sort);

	if (!isnan (target_score)) {
		msg_info_task ("<%s>: set pre-result to %s (%.2f): '%s' from %s(%d)",
				task->message_id, rspamd_action_to_str (action), target_score,
				message, module, priority);
	}

	else {
		msg_info_task ("<%s>: set pre-result to %s (no score): '%s' from %s(%d)",
				task->message_id, rspamd_action_to_str (action),
				message, module, priority);
	}
}

static inline gdouble
rspamd_check_group_score (struct rspamd_task *task,
		const gchar *symbol,
		struct rspamd_symbols_group *gr,
		gdouble *group_score,
		gdouble w)
{
	if (gr != NULL && group_score && gr->max_score > 0.0 && w > 0.0) {
		if (*group_score >= gr->max_score && w > 0) {
			msg_info_task ("maximum group score %.2f for group %s has been reached,"
						   " ignoring symbol %s with weight %.2f", gr->max_score,
					gr->name, symbol, w);
			return NAN;
		}
		else if (*group_score + w > gr->max_score) {
			w = gr->max_score - *group_score;
		}
	}

	return w;
}

#ifndef DBL_EPSILON
#define DBL_EPSILON 2.2204460492503131e-16
#endif

static struct rspamd_symbol_result *
insert_metric_result (struct rspamd_task *task,
		const gchar *symbol,
		double weight,
		const gchar *opt,
		enum rspamd_symbol_insert_flags flags)
{
	struct rspamd_metric_result *metric_res;
	struct rspamd_symbol_result *s = NULL;
	gdouble final_score, *gr_score = NULL, next_gf = 1.0, diff;
	struct rspamd_symbol *sdef;
	struct rspamd_symbols_group *gr = NULL;
	const ucl_object_t *mobj, *sobj;
	gint max_shots, ret;
	guint i;
	khiter_t k;
	gboolean single = !!(flags & RSPAMD_SYMBOL_INSERT_SINGLE);
	gchar *sym_cpy;

	metric_res = task->result;

	if (!isfinite (weight)) {
		msg_warn_task ("detected %s score for symbol %s, replace it with zero",
				isnan (weight) ? "NaN" : "infinity", symbol);
		weight = 0.0;
	}

	sdef = g_hash_table_lookup (task->cfg->symbols, symbol);
	if (sdef == NULL) {
		if (flags & RSPAMD_SYMBOL_INSERT_ENFORCE) {
			final_score = 1.0 * weight; /* Enforce static weight to 1.0 */
		}
		else {
			final_score = 0.0;
		}
	}
	else {
		final_score = (*sdef->weight_ptr) * weight;

		PTR_ARRAY_FOREACH (sdef->groups, i, gr) {
			k = kh_get (rspamd_symbols_group_hash, metric_res->sym_groups, gr);

			if (k == kh_end (metric_res->sym_groups)) {
				k = kh_put (rspamd_symbols_group_hash, metric_res->sym_groups,
						gr, &ret);
				kh_value (metric_res->sym_groups, k) = 0;
			}
		}
	}

	if (task->settings) {
		mobj = task->settings;
		gdouble corr;

		sobj = ucl_object_lookup (mobj, symbol);
		if (sobj != NULL && ucl_object_todouble_safe (sobj, &corr)) {
			msg_debug ("settings: changed weight of symbol %s from %.2f to %.2f",
					symbol, final_score, corr);
			final_score = corr * weight;
		}
	}

	/* Add metric score */
	k = kh_get (rspamd_symbols_hash, metric_res->symbols, symbol);
	if (k != kh_end (metric_res->symbols)) {
		s = &kh_value (metric_res->symbols, k);
		if (single) {
			max_shots = 1;
		}
		else {
			if (sdef) {
				max_shots = sdef->nshots;
			}
			else {
				max_shots = task->cfg->default_max_shots;
			}
		}

		if (!single && (max_shots > 0 && (s->nshots >= max_shots))) {
			single = TRUE;
		}

		/* Now check for the duplicate options */
		if (opt && s->options) {
			k = kh_get (rspamd_options_hash, s->options, opt);

			if (k == kh_end (s->options)) {
				rspamd_task_add_result_option (task, s, opt);
			}
			else {
				s->nshots ++;
			}
		}
		else {
			s->nshots ++;
			rspamd_task_add_result_option (task, s, opt);
		}

		/* Adjust diff */
		if (!single) {
			diff = final_score;
		}
		else {
			if (fabs (s->score) < fabs (final_score) &&
				signbit (s->score) == signbit (final_score)) {
				/* Replace less significant weight with a more significant one */
				diff = final_score - s->score;
			}
			else {
				diff = 0;
			}
		}

		if (diff) {
			/* Handle grow factor */
			if (metric_res->grow_factor && diff > 0) {
				diff *= metric_res->grow_factor;
				next_gf *= task->cfg->grow_factor;
			}
			else if (diff > 0) {
				next_gf = task->cfg->grow_factor;
			}

			if (sdef) {
				PTR_ARRAY_FOREACH (sdef->groups, i, gr) {
					gdouble cur_diff;

					k = kh_get (rspamd_symbols_group_hash,
							metric_res->sym_groups, gr);
					g_assert (k != kh_end (metric_res->sym_groups));
					gr_score = &kh_value (metric_res->sym_groups, k);
					cur_diff = rspamd_check_group_score (task, symbol, gr,
							gr_score, diff);

					if (isnan (cur_diff)) {
						/* Limit reached, do not add result */
						diff = NAN;
						break;
					} else if (gr_score) {
						*gr_score += cur_diff;

						if (cur_diff < diff) {
							/* Reduce */
							diff = cur_diff;
						}
					}
				}
			}

			if (!isnan (diff)) {
				metric_res->score += diff;
				metric_res->grow_factor = next_gf;

				if (single) {
					s->score = final_score;
				} else {
					s->score += diff;
				}
			}
		}
	}
	else {
		sym_cpy = rspamd_mempool_strdup (task->task_pool, symbol);
		k = kh_put (rspamd_symbols_hash, metric_res->symbols,
				sym_cpy, &ret);
		g_assert (ret > 0);
		s = &kh_value (metric_res->symbols, k);
		memset (s, 0, sizeof (*s));

		/* Handle grow factor */
		if (metric_res->grow_factor && final_score > 0) {
			final_score *= metric_res->grow_factor;
			next_gf *= task->cfg->grow_factor;
		}
		else if (final_score > 0) {
			next_gf = task->cfg->grow_factor;
		}

		s->name = sym_cpy;
		s->sym = sdef;
		s->nshots = 1;

		if (sdef) {
			/* Check group limits */
			PTR_ARRAY_FOREACH (sdef->groups, i, gr) {
				gdouble cur_score;

				k = kh_get (rspamd_symbols_group_hash, metric_res->sym_groups, gr);
				g_assert (k != kh_end (metric_res->sym_groups));
				gr_score = &kh_value (metric_res->sym_groups, k);
				cur_score = rspamd_check_group_score (task, symbol, gr,
						gr_score, final_score);

				if (isnan (cur_score)) {
					/* Limit reached, do not add result */
					final_score = NAN;
					break;
				} else if (gr_score) {
					*gr_score += cur_score;

					if (cur_score < final_score) {
						/* Reduce */
						final_score = cur_score;
					}
				}
			}
		}

		if (!isnan (final_score)) {
			const double epsilon = DBL_EPSILON;

			metric_res->score += final_score;
			metric_res->grow_factor = next_gf;
			s->score = final_score;

			if (final_score > epsilon) {
				metric_res->npositive ++;
				metric_res->positive_score += final_score;
			}
			else if (final_score < -epsilon) {
				metric_res->nnegative ++;
				metric_res->negative_score += fabs (final_score);
			}
		}
		else {
			s->score = 0;
		}

		rspamd_task_add_result_option (task, s, opt);
	}

	msg_debug_task ("symbol %s, score %.2f, factor: %f",
			symbol,
			s->score,
			final_score);

	return s;
}

struct rspamd_symbol_result *
rspamd_task_insert_result_full (struct rspamd_task *task,
		const gchar *symbol,
		double weight,
		const gchar *opt,
		enum rspamd_symbol_insert_flags flags)
{
	struct rspamd_symbol_result *s = NULL;

	if (task->processed_stages & (RSPAMD_TASK_STAGE_IDEMPOTENT >> 1)) {
		msg_err_task ("cannot insert symbol %s on idempotent phase",
				symbol);

		return NULL;
	}

	/* Insert symbol to default metric */
	s = insert_metric_result (task,
			symbol,
			weight,
			opt,
			flags);

	/* Process cache item */
	if (task->cfg->cache) {
		rspamd_symbols_cache_inc_frequency (task->cfg->cache, symbol);
	}

	return s;
}

gboolean
rspamd_task_add_result_option (struct rspamd_task *task,
		struct rspamd_symbol_result *s, const gchar *val)
{
	struct rspamd_symbol_option *opt;
	gboolean ret = FALSE;
	gchar *opt_cpy;
	khiter_t k;
	gint r;

	if (s && val) {
		if (s->options && !(s->sym &&
				(s->sym->flags & RSPAMD_SYMBOL_FLAG_ONEPARAM)) &&
				kh_size (s->options) < task->cfg->default_max_shots) {
			/* Append new options */
			k = kh_get (rspamd_options_hash, s->options, val);

			if (k == kh_end (s->options)) {
				opt = rspamd_mempool_alloc0 (task->task_pool, sizeof (*opt));
				opt_cpy = rspamd_mempool_strdup (task->task_pool, val);
				k = kh_put (rspamd_options_hash, s->options, opt_cpy, &r);

				kh_value (s->options, k) = opt;
				opt->option = opt_cpy;
				DL_APPEND (s->opts_head, opt);

				ret = TRUE;
			}
		}
		else {
			s->options = kh_init (rspamd_options_hash);
			opt = rspamd_mempool_alloc0 (task->task_pool, sizeof (*opt));
			opt_cpy = rspamd_mempool_strdup (task->task_pool, val);
			k = kh_put (rspamd_options_hash, s->options, opt_cpy, &r);

			kh_value (s->options, k) = opt;
			opt->option = opt_cpy;
			DL_APPEND (s->opts_head, opt);

			ret = TRUE;
		}
	}
	else if (!val) {
		ret = TRUE;
	}

	return ret;
}

enum rspamd_action_type
rspamd_check_action_metric (struct rspamd_task *task, struct rspamd_metric_result *mres)
{
	struct rspamd_action *action, *selected_action = NULL;
	struct rspamd_passthrough_result *pr;
	double max_score = -(G_MAXDOUBLE), sc;
	int i;
	gboolean set_action = FALSE;

	/* We are not certain about the results during processing */
	if (task->result->passthrough_result == NULL) {
		for (i = METRIC_ACTION_REJECT; i < METRIC_ACTION_MAX; i++) {
			action = &task->cfg->actions[i];
			sc = mres->actions_limits[i];

			if (isnan (sc)) {
				continue;
			}

			if (mres->score >= sc && sc > max_score) {
				selected_action = action;
				max_score = sc;
			}
		}

		if (set_action && selected_action == NULL) {
			selected_action = &task->cfg->actions[METRIC_ACTION_NOACTION];
		}
	}
	else {
		/* Peek the highest priority result */
		pr = task->result->passthrough_result;
		sc = pr->target_score;
		selected_action = &task->cfg->actions[pr->action];

		if (!isnan (sc)) {
			if (pr->action == METRIC_ACTION_NOACTION) {
				mres->score = MIN (sc, mres->score);
			}
			else {
				mres->score = sc;
			}
		}
	}

	if (selected_action) {
		return selected_action->action;
	}

	return METRIC_ACTION_NOACTION;
}

struct rspamd_symbol_result*
rspamd_task_find_symbol_result (struct rspamd_task *task, const char *sym)
{
	struct rspamd_symbol_result *res = NULL;
	khiter_t k;


	if (task->result) {
		k = kh_get (rspamd_symbols_hash, task->result->symbols, sym);

		if (k != kh_end (task->result->symbols)) {
			res = &kh_value (task->result->symbols, k);
		}
	}

	return res;
}

void
rspamd_task_symbol_result_foreach (struct rspamd_task *task,
										GHFunc func,
										gpointer ud)
{
	const gchar *kk;
	struct rspamd_symbol_result res;

	if (func && task->result) {
		kh_foreach (task->result->symbols, kk, res, {
			func ((gpointer)kk, (gpointer)&res, ud);
		});
	}
}