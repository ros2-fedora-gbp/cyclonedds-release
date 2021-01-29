/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <string.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/log.h"
#include "dds__entity.h"
#include "dds__write.h"
#include "dds__writer.h"
#include "dds__reader.h"
#include "dds__listener.h"
#include "dds__qos.h"
#include "dds__topic.h"
#include "dds/version.h"
#include "dds/ddsi/ddsi_pmd.h"
#include "dds/ddsi/ddsi_xqos.h"
#include "dds/ddsi/q_transmit.h"
#include "dds/ddsi/q_bswap.h"

extern inline dds_entity *dds_entity_from_handle_link (struct dds_handle_link *hdllink);
extern inline bool dds_entity_is_enabled (const dds_entity *e);
extern inline void dds_entity_status_reset (dds_entity *e, status_mask_t t);
extern inline dds_entity_kind_t dds_entity_kind (const dds_entity *e);

const struct dds_entity_deriver *dds_entity_deriver_table[] = {
  [DDS_KIND_TOPIC] = &dds_entity_deriver_topic,
  [DDS_KIND_PARTICIPANT] = &dds_entity_deriver_participant,
  [DDS_KIND_READER] = &dds_entity_deriver_reader,
  [DDS_KIND_WRITER] = &dds_entity_deriver_writer,
  [DDS_KIND_SUBSCRIBER] = &dds_entity_deriver_subscriber,
  [DDS_KIND_PUBLISHER] = &dds_entity_deriver_publisher,
  [DDS_KIND_COND_READ] = &dds_entity_deriver_readcondition,
  [DDS_KIND_COND_QUERY] = &dds_entity_deriver_readcondition,
  [DDS_KIND_COND_GUARD] = &dds_entity_deriver_guardcondition,
  [DDS_KIND_WAITSET] = &dds_entity_deriver_waitset,
  [DDS_KIND_DOMAIN] = &dds_entity_deriver_domain,
  [DDS_KIND_CYCLONEDDS] = &dds_entity_deriver_cyclonedds
};

void dds_entity_deriver_dummy_interrupt (struct dds_entity *e) {
  (void) e;
}
void dds_entity_deriver_dummy_close (struct dds_entity *e) {
  (void) e;
}
dds_return_t dds_entity_deriver_dummy_delete (struct dds_entity *e) {
  (void) e; return DDS_RETCODE_OK;
}
dds_return_t dds_entity_deriver_dummy_set_qos (struct dds_entity *e, const dds_qos_t *qos, bool enabled) {
  (void) e; (void) qos; (void) enabled; return DDS_RETCODE_ILLEGAL_OPERATION;
}
dds_return_t dds_entity_deriver_dummy_validate_status (uint32_t mask) {
  (void) mask; return DDS_RETCODE_ILLEGAL_OPERATION;
}
struct dds_statistics *dds_entity_deriver_dummy_create_statistics (const struct dds_entity *e) {
  (void) e; return NULL;
}
void dds_entity_deriver_dummy_refresh_statistics (const struct dds_entity *e, struct dds_statistics *s) {
  (void) e; (void) s;
}

extern inline void dds_entity_deriver_interrupt (struct dds_entity *e);
extern inline void dds_entity_deriver_close (struct dds_entity *e);
extern inline dds_return_t dds_entity_deriver_delete (struct dds_entity *e);
extern inline dds_return_t dds_entity_deriver_set_qos (struct dds_entity *e, const dds_qos_t *qos, bool enabled);
extern inline dds_return_t dds_entity_deriver_validate_status (struct dds_entity *e, uint32_t mask);
extern inline bool dds_entity_supports_set_qos (struct dds_entity *e);
extern inline bool dds_entity_supports_validate_status (struct dds_entity *e);
extern inline struct dds_statistics *dds_entity_deriver_create_statistics (const struct dds_entity *e);
extern inline void dds_entity_deriver_refresh_statistics (const struct dds_entity *e, struct dds_statistics *s);

static int compare_instance_handle (const void *va, const void *vb)
{
  const dds_instance_handle_t *a = va;
  const dds_instance_handle_t *b = vb;
  return (*a == *b) ? 0 : (*a < *b) ? -1 : 1;
}

const ddsrt_avl_treedef_t dds_entity_children_td = DDSRT_AVL_TREEDEF_INITIALIZER (offsetof (struct dds_entity, m_avlnode_child), offsetof (struct dds_entity, m_iid), compare_instance_handle, 0);

static void dds_entity_observers_signal (dds_entity *observed, uint32_t status);
static void dds_entity_observers_signal_delete (dds_entity *observed);

static dds_return_t dds_delete_impl (dds_entity_t entity, enum delete_impl_state delstate);
static dds_return_t really_delete_pinned_closed_locked (struct dds_entity *e, enum delete_impl_state delstate);

void dds_entity_add_ref_locked (dds_entity *e)
{
  dds_handle_add_ref (&e->m_hdllink);
}

void dds_entity_drop_ref (dds_entity *e)
{
  if (dds_handle_drop_ref (&e->m_hdllink))
  {
    dds_return_t ret = dds_delete_impl(e->m_hdllink.hdl, DIS_EXPLICIT);
    assert (ret == DDS_RETCODE_OK);
    (void) ret;
  }
}

void dds_entity_unpin_and_drop_ref (dds_entity *e)
{
  if (dds_handle_unpin_and_drop_ref (&e->m_hdllink))
  {
    dds_return_t ret = dds_delete_impl(e->m_hdllink.hdl, DIS_EXPLICIT);
    assert (ret == DDS_RETCODE_OK);
    (void) ret;
  }
}

static bool entity_has_status (const dds_entity *e)
{
  switch (e->m_kind)
  {
    case DDS_KIND_TOPIC:
    case DDS_KIND_READER:
    case DDS_KIND_WRITER:
    case DDS_KIND_PUBLISHER:
    case DDS_KIND_SUBSCRIBER:
    case DDS_KIND_PARTICIPANT:
      return true;
    case DDS_KIND_COND_READ:
    case DDS_KIND_COND_QUERY:
    case DDS_KIND_COND_GUARD:
    case DDS_KIND_WAITSET:
    case DDS_KIND_DOMAIN:
    case DDS_KIND_CYCLONEDDS:
      break;
    case DDS_KIND_DONTCARE:
      abort ();
      break;
  }
  return false;
}

static bool entity_may_have_children (const dds_entity *e)
{
  switch (e->m_kind)
  {
    case DDS_KIND_TOPIC:
      return false;
    case DDS_KIND_READER:
    case DDS_KIND_WRITER:
    case DDS_KIND_PUBLISHER:
    case DDS_KIND_SUBSCRIBER:
    case DDS_KIND_PARTICIPANT:
    case DDS_KIND_COND_READ:
    case DDS_KIND_COND_QUERY:
    case DDS_KIND_COND_GUARD:
    case DDS_KIND_WAITSET:
    case DDS_KIND_DOMAIN:
    case DDS_KIND_CYCLONEDDS:
      break;
    case DDS_KIND_DONTCARE:
      abort ();
      break;
  }
  return true;
}

#ifndef NDEBUG
static bool entity_kind_has_qos (dds_entity_kind_t kind)
{
  switch (kind)
  {
    case DDS_KIND_READER:
    case DDS_KIND_WRITER:
    case DDS_KIND_PUBLISHER:
    case DDS_KIND_SUBSCRIBER:
    case DDS_KIND_PARTICIPANT:
      return true;
    case DDS_KIND_TOPIC:
    case DDS_KIND_COND_READ:
    case DDS_KIND_COND_QUERY:
    case DDS_KIND_COND_GUARD:
    case DDS_KIND_WAITSET:
    case DDS_KIND_DOMAIN:
    case DDS_KIND_CYCLONEDDS:
      break;
    case DDS_KIND_DONTCARE:
      abort ();
      break;
  }
  return false;
}
#endif

dds_entity_t dds_entity_init (dds_entity *e, dds_entity *parent, dds_entity_kind_t kind, bool implicit, dds_qos_t *qos, const dds_listener_t *listener, status_mask_t mask)
{
  dds_handle_t handle;

  /* CycloneDDS is at the root of the hierarchy */
  assert ((kind == DDS_KIND_CYCLONEDDS) == (parent == NULL));
  assert (entity_kind_has_qos (kind) == (qos != NULL));
  assert (e);

  e->m_kind = kind;
  e->m_qos = qos;
  e->m_cb_count = 0;
  e->m_cb_pending_count = 0;
  e->m_observers = NULL;

  /* TODO: CHAM-96: Implement dynamic enabling of entity. */
  e->m_flags |= DDS_ENTITY_ENABLED;
  if (implicit)
    e->m_flags |= DDS_ENTITY_IMPLICIT;

  /* set the status enable based on kind */
  if (entity_has_status (e))
    ddsrt_atomic_st32 (&e->m_status.m_status_and_mask, (uint32_t) mask << SAM_ENABLED_SHIFT);
  else
    ddsrt_atomic_st32 (&e->m_status.m_trigger, 0);

  ddsrt_mutex_init (&e->m_mutex);
  ddsrt_mutex_init (&e->m_observers_lock);
  ddsrt_cond_init (&e->m_cond);
  ddsrt_cond_init (&e->m_observers_cond);

  if (parent)
  {
    e->m_parent = parent;
    e->m_domain = parent->m_domain;
  }
  else
  {
    assert (kind == DDS_KIND_CYCLONEDDS);
    e->m_parent = NULL;
    e->m_domain = NULL;
  }
  ddsrt_avl_init (&dds_entity_children_td, &e->m_children);

  dds_reset_listener (&e->m_listener);
  if (listener)
    dds_merge_listener (&e->m_listener, listener);
  if (parent)
  {
    ddsrt_mutex_lock (&parent->m_observers_lock);
    dds_inherit_listener (&e->m_listener, &parent->m_listener);
    ddsrt_mutex_unlock (&parent->m_observers_lock);
  }

  if (kind == DDS_KIND_CYCLONEDDS)
  {
    if ((handle = dds_handle_register_special (&e->m_hdllink, implicit, true, DDS_CYCLONEDDS_HANDLE)) <= 0)
      return (dds_entity_t) handle;
  }
  else
  {
    /* for topics, refc counts readers/writers, for all others, it counts children (this we can get away with
       as long as topics can't have children) */
    if ((handle = dds_handle_create (&e->m_hdllink, implicit, entity_may_have_children (e))) <= 0)
      return (dds_entity_t) handle;
  }

  /* An dds_handle_t is directly used as dds_entity_t. */
  return (dds_entity_t) handle;
}

void dds_entity_init_complete (dds_entity *entity)
{
  dds_handle_unpend (&entity->m_hdllink);
}

void dds_entity_register_child (dds_entity *parent, dds_entity *child)
{
  /* parent must be tracking children in its refc, or children can't be added */
  assert (ddsrt_atomic_ld32 (&parent->m_hdllink.cnt_flags) & HDL_FLAG_ALLOW_CHILDREN);
  assert (child->m_iid != 0);
  assert (ddsrt_avl_lookup (&dds_entity_children_td, &parent->m_children, &child->m_iid) == NULL);
  ddsrt_avl_insert (&dds_entity_children_td, &parent->m_children, child);
  dds_entity_add_ref_locked (parent);
}

static dds_entity *get_next_child (ddsrt_avl_tree_t *remaining_children, uint32_t allowed_kinds, uint64_t *cursor)
{
  ddsrt_avl_iter_t it;
  for (dds_entity *e = ddsrt_avl_iter_succ (&dds_entity_children_td, remaining_children, &it, cursor); e != NULL; e = ddsrt_avl_iter_next (&it))
  {
    dds_entity_kind_t kind = dds_entity_kind (e);
    if ((1u << (uint32_t) kind) & allowed_kinds)
      return e;
  }
  return NULL;
}

static void delete_children (struct dds_entity *parent, uint32_t allowed_kinds)
{
  dds_entity *child;
  dds_return_t ret;
  uint64_t cursor = 0;
  ddsrt_mutex_lock (&parent->m_mutex);
  while ((child = get_next_child (&parent->m_children, allowed_kinds, &cursor)) != NULL)
  {
    dds_entity_t child_handle = child->m_hdllink.hdl;
    cursor = child->m_iid;

    /* The child will remove itself from the parent->m_children list. */
    ddsrt_mutex_unlock (&parent->m_mutex);
    ret = dds_delete_impl (child_handle, DIS_FROM_PARENT);
    assert (ret == DDS_RETCODE_OK || ret == DDS_RETCODE_BAD_PARAMETER);
    (void) ret;
    ddsrt_mutex_lock (&parent->m_mutex);

    /* The dds_delete can fail if the child is being deleted in parallel, in which case:
       wait until it is has gone. */
    if (ddsrt_avl_lookup (&dds_entity_children_td, &parent->m_children, &cursor) != NULL)
      ddsrt_cond_wait (&parent->m_cond, &parent->m_mutex);
  }
  ddsrt_mutex_unlock (&parent->m_mutex);
}

#define TRACE_DELETE 0 /* FIXME: use DDS_LOG for this */
#if TRACE_DELETE
static const char *entity_kindstr (dds_entity_kind_t kind)
{
  switch (kind)
  {
    case DDS_KIND_TOPIC: return "topic";
    case DDS_KIND_READER: return "reader";
    case DDS_KIND_WRITER: return "writer";
    case DDS_KIND_PUBLISHER: return "publisher";
    case DDS_KIND_SUBSCRIBER: return "subscriber";
    case DDS_KIND_PARTICIPANT: return "participant";
    case DDS_KIND_COND_READ: return "readcond";
    case DDS_KIND_COND_QUERY: return "querycond";
    case DDS_KIND_COND_GUARD: return "guardcond";
    case DDS_KIND_WAITSET: return "waitset";
    case DDS_KIND_DOMAIN: return "domain";
    case DDS_KIND_CYCLONEDDS: return "cyclonedds";
    case DDS_KIND_DONTCARE: break;
  }
  return "UNDEF";
}

static void print_delete (const dds_entity *e, enum delete_impl_state delstate , dds_instance_handle_t iid)
{
  if (e)
  {
    unsigned cm = ddsrt_atomic_ld32 (&e->m_hdllink.cnt_flags);
    printf ("delete(%p, delstate %s, iid %"PRIx64"): %s%s %d pin %u refc %u %s %s\n",
            (void *) e, (delstate == DIS_IMPLICIT) ? "implicit" : (delstate == DIS_EXPLICIT) ? "explicit" : "from_parent", iid,
            entity_kindstr (e->m_kind), (e->m_flags & DDS_ENTITY_IMPLICIT) ? " [implicit]" : "",
            e->m_hdllink.hdl, cm & 0xfff, (cm >> 12) & 0x7fff, (cm & 0x80000000) ? "closed" : "open",
            ddsrt_avl_is_empty (&e->m_children) ? "childless" : "has-children");
  }
  else
  {
    printf ("delete(%p, delstate %s, handle %"PRId64"): pin failed\n",
            (void *) e, (delstate == DIS_IMPLICIT) ? "implicit" : (delstate == DIS_EXPLICIT) ? "explicit" : "from_parent", iid);
  }
}
#endif

dds_return_t dds_delete (dds_entity_t entity)
{
  return dds_delete_impl (entity, DIS_EXPLICIT);
}

void dds_entity_final_deinit_before_free (dds_entity *e)
{
  dds_delete_qos (e->m_qos);
  ddsrt_cond_destroy (&e->m_cond);
  ddsrt_cond_destroy (&e->m_observers_cond);
  ddsrt_mutex_destroy (&e->m_mutex);
  ddsrt_mutex_destroy (&e->m_observers_lock);
}

static dds_return_t dds_delete_impl (dds_entity_t entity, enum delete_impl_state delstate)
{
  dds_entity *e;
  dds_return_t ret;
  if ((ret = dds_entity_pin_for_delete (entity, (delstate != DIS_IMPLICIT), &e)) == DDS_RETCODE_OK)
    return dds_delete_impl_pinned (e, delstate);
  else if (ret == DDS_RETCODE_TRY_AGAIN) /* non-child refs exist */
    return DDS_RETCODE_OK;
  else
  {
#if TRACE_DELETE
    print_delete (NULL, delstate, (uint64_t) entity);
#endif
    return ret;
  }
}

dds_return_t dds_delete_impl_pinned (dds_entity *e, enum delete_impl_state delstate)
{
  /* Any number of threads pinning it, possibly in delete, or having pinned it and
     trying to acquire m_mutex */

  ddsrt_mutex_lock (&e->m_mutex);
#if TRACE_DELETE
  print_delete (e, delstate, e->m_iid);
#endif

  /* If another thread was racing us in delete, it will have set the CLOSING flag
     while holding m_mutex and we had better bail out. */
  assert (dds_handle_is_closed (&e->m_hdllink));
  return really_delete_pinned_closed_locked (e, delstate);
}

static dds_return_t really_delete_pinned_closed_locked (struct dds_entity *e, enum delete_impl_state delstate)
{
  dds_return_t ret;

  /* No threads pinning it anymore, no need to worry about other threads deleting
     it, but there can still be plenty of threads that have it pinned and are
     trying to acquire m_mutex to do their thing (including creating children,
     attaching to waitsets, &c.) */

  assert (dds_handle_is_closed (&e->m_hdllink));

  /* Trigger blocked threads (and, still, delete DDSI reader/writer to trigger
     continued cleanup -- while that's quite safe given that GUIDs don't get
     reused quickly, it needs an update) */
  dds_entity_deriver_interrupt (e);

  /* Prevent further listener invocations; dds_set_status_mask locks m_mutex and
     checks for a pending delete to guarantee that once we clear the mask here,
     no new listener invocations will occur beyond those already in progress.
     (FIXME: or committed to?  I think in-progress only, better check.) */
  ddsrt_mutex_lock (&e->m_observers_lock);
  if (entity_has_status (e))
    ddsrt_atomic_and32 (&e->m_status.m_status_and_mask, SAM_STATUS_MASK);

  ddsrt_mutex_unlock (&e->m_mutex);

  /* wait for all listeners to complete - FIXME: rely on pincount instead?
     that would require all listeners to pin the entity instead, but it
     would prevent them from doing much. */
  while (e->m_cb_pending_count > 0)
    ddsrt_cond_wait (&e->m_observers_cond, &e->m_observers_lock);
  ddsrt_mutex_unlock (&e->m_observers_lock);

  /* Wait for all other threads to unpin the entity */
  dds_handle_close_wait (&e->m_hdllink);

  /* Pin count dropped to one with CLOSING flag set: no other threads still
     in operations involving this entity */
  dds_entity_observers_signal_delete (e);
  dds_entity_deriver_close (e);

  /*
   * Recursively delete children.
   *
   * It is possible that a writer/reader has the last reference
   * to a topic. This will mean that when deleting a writer could
   * cause a topic to be deleted.
   * This can cause issues when deleting the children of a participant:
   * when a topic is the next child in line to be deleted, while at the
   * same time it is already being deleted due to the recursive deletion
   * of a publisher->writer.
   *
   * Another problem is that when the topic was already deleted, and
   * we'd delete it here for the second time before the writer/reader
   * is deleted, they will have dangling pointers.
   *
   * To circumvent the problem. We ignore topics in the first loop.
   */
  DDSRT_STATIC_ASSERT ((uint32_t) DDS_KIND_MAX < 32);
  static const uint32_t disallowed_kinds[] = {
    1u << (uint32_t) DDS_KIND_TOPIC,
    (uint32_t) 0
  };
  for (size_t i = 0; i < sizeof (disallowed_kinds) / sizeof (disallowed_kinds[0]); i++)
  {
    delete_children (e, ~disallowed_kinds[i]);
  }

  /* The dds_handle_delete will wait until the last active claim on that handle is
     released. It is possible that this last release will be done by a thread that was
     kicked during the close(). */
  ret = dds_handle_delete (&e->m_hdllink);
  assert (ret == DDS_RETCODE_OK);
  (void) ret;

  /* Remove from parent; schedule deletion of parent if it was created implicitly, no
     longer has any remaining children, and we didn't arrive here as a consequence of
     deleting the parent. */
  dds_entity *parent_to_delete = NULL;
  if (e->m_parent != NULL)
  {
    struct dds_entity * const p = e->m_parent;

    ddsrt_mutex_lock (&p->m_mutex);
    assert (ddsrt_avl_lookup (&dds_entity_children_td, &p->m_children, &e->m_iid) != NULL);
    ddsrt_avl_delete (&dds_entity_children_td, &p->m_children, e);
    if (dds_handle_drop_childref_and_pin (&p->m_hdllink, delstate != DIS_FROM_PARENT))
    {
      dds_handle_close(&p->m_hdllink);
      assert (dds_handle_is_closed (&p->m_hdllink));
      assert (dds_handle_is_not_refd (&p->m_hdllink));
      assert (ddsrt_avl_is_empty (&p->m_children));
      parent_to_delete = p;
    }
    /* trigger parent in case it is waiting in delete */
    ddsrt_cond_broadcast (&p->m_cond);
    ddsrt_mutex_unlock (&p->m_mutex);
  }

  /* Do some specific deletion when needed */
  ret = dds_entity_deriver_delete (e);
  if (ret == DDS_RETCODE_NO_DATA)
  {
    /* Bootstrapping and its inverse are always a tricky business, and here it is no different:
       deleting the pseudo-top-level object tears down all kinds of stuff that is supposed to
       remain in existence (like the entire platform abstraction) and so it must be the final
       call.  Thus, we rely on it to call "dds_entity_final_deinit_before_free" and return a
       special error code that we don't pass on to the caller. */
    ret = DDS_RETCODE_OK;
  }
  else if (ret != DDS_RETCODE_OK)
  {
    if (parent_to_delete)
      dds_entity_unpin (parent_to_delete);
    return ret;
  }
  else
  {
    dds_entity_final_deinit_before_free (e);
    dds_free (e);
  }

  assert (ret == DDS_RETCODE_OK);
  (void) ret;
  return (parent_to_delete != NULL) ? dds_delete_impl_pinned (parent_to_delete, DIS_IMPLICIT) : DDS_RETCODE_OK;
}

bool dds_entity_in_scope (const dds_entity *e, const dds_entity *root)
{
  /* An entity is not an ancestor of itself */
  while (e != NULL && e != root)
    e = e->m_parent;
  return (e != NULL);
}

dds_entity_t dds_get_parent (dds_entity_t entity)
{
  dds_entity *e;
  dds_return_t rc;
  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    dds_entity_t hdl = e->m_parent ? e->m_parent->m_hdllink.hdl : 0;
    dds_entity_unpin (e);
    return hdl;
  }
}

dds_participant *dds_entity_participant (const dds_entity *e)
{
  while (e && dds_entity_kind (e) != DDS_KIND_PARTICIPANT)
    e = e->m_parent;
  return (dds_participant *) e;
}

const ddsi_guid_t *dds_entity_participant_guid (const dds_entity *e)
{
  struct dds_participant const * const pp = dds_entity_participant (e);
  assert (pp != NULL);
  return &pp->m_entity.m_guid;
}

dds_entity_t dds_get_participant (dds_entity_t entity)
{
  dds_entity *e;
  dds_return_t rc;
  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    dds_participant *par = dds_entity_participant (e);
    dds_entity_t hdl = par ? par->m_entity.m_hdllink.hdl : 0;
    dds_entity_unpin (e);
    return hdl;
  }
}

dds_return_t dds_get_children (dds_entity_t entity, dds_entity_t *children, size_t size)
{
  dds_entity *e;
  dds_return_t rc;

  if ((children != NULL && (size == 0 || size > INT32_MAX)) || (children == NULL && size != 0))
    return DDS_RETCODE_BAD_PARAMETER;

  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    ddsrt_avl_iter_t it;
    size_t n = 0;
    struct dds_entity *c;
    ddsrt_mutex_lock (&e->m_mutex);
    for (c = ddsrt_avl_iter_first (&dds_entity_children_td, &e->m_children, &it); c != NULL; c = ddsrt_avl_iter_next (&it))
    {
      struct dds_entity *tmp;
      /* Attempt at pinning the entity will fail if it is still pending */
      if (dds_entity_pin (c->m_hdllink.hdl, &tmp) == DDS_RETCODE_OK)
      {
        if (n < size)
          children[n] = c->m_hdllink.hdl;
        n++;
        dds_entity_unpin (tmp);
      }
    }
    ddsrt_mutex_unlock (&e->m_mutex);
    dds_entity_unpin (e);
    /* there are fewer than INT32_MAX handles, so there can never be more entities */
    assert (n <= INT32_MAX);
    return (dds_return_t) n;
  }
}

static uint64_t entity_kind_qos_mask (dds_entity_kind_t kind)
{
  switch (kind)
  {
    case DDS_KIND_TOPIC:
      return DDS_TOPIC_QOS_MASK;
    case DDS_KIND_PARTICIPANT:
      return DDS_PARTICIPANT_QOS_MASK;
    case DDS_KIND_READER:
      return DDS_READER_QOS_MASK;
    case DDS_KIND_WRITER:
      return DDS_WRITER_QOS_MASK;
    case DDS_KIND_SUBSCRIBER:
      return DDS_SUBSCRIBER_QOS_MASK;
    case DDS_KIND_PUBLISHER:
      return DDS_PUBLISHER_QOS_MASK;
    case DDS_KIND_DONTCARE:
    case DDS_KIND_COND_READ:
    case DDS_KIND_COND_QUERY:
    case DDS_KIND_COND_GUARD:
    case DDS_KIND_WAITSET:
    case DDS_KIND_DOMAIN:
    case DDS_KIND_CYCLONEDDS:
      break;
  }
  return 0;
}

dds_return_t dds_get_qos (dds_entity_t entity, dds_qos_t *qos)
{
  dds_entity *e;
  dds_return_t ret;

  if (qos == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return ret;

  if (!dds_entity_supports_set_qos (e))
    ret = DDS_RETCODE_ILLEGAL_OPERATION;
  else
  {
    dds_qos_t *entity_qos;
    if (dds_entity_kind (e) != DDS_KIND_TOPIC)
      entity_qos = e->m_qos;
    else
    {
      struct dds_topic * const tp = (dds_topic *) e;
      struct dds_participant * const pp = dds_entity_participant (e);
      ddsrt_mutex_lock (&pp->m_entity.m_mutex);
      entity_qos = tp->m_ktopic->qos;
      ddsrt_mutex_unlock (&pp->m_entity.m_mutex);
    }

    dds_reset_qos (qos);
    ddsi_xqos_mergein_missing (qos, entity_qos, ~(QP_TOPIC_NAME | QP_TYPE_NAME));
    ret = DDS_RETCODE_OK;
  }
  dds_entity_unlock(e);
  return ret;
}

static dds_return_t dds_set_qos_locked_raw (dds_entity *e, dds_qos_t **e_qos_ptr, const dds_qos_t *qos, uint64_t mask, const struct ddsrt_log_cfg *logcfg)
{
  const bool enabled = ((e->m_flags & DDS_ENTITY_ENABLED) != 0);
  dds_return_t ret;

  /* Any attempt to do this on a topic ends up doing it on the ktopic instead, so that there is
     but a single QoS for a topic in a participant while there can be multiple definitions of it,
     and hence, multiple sertopics.  Those are needed for multi-language support. */
  dds_qos_t *newqos = dds_create_qos ();
  ddsi_xqos_mergein_missing (newqos, qos, mask);
  ddsi_xqos_mergein_missing (newqos, *e_qos_ptr, ~(uint64_t)0);
  if ((ret = ddsi_xqos_valid (logcfg, newqos)) != DDS_RETCODE_OK)
  {
    /* invalid or inconsistent QoS settings */
    goto error_or_nochange;
  }
  else if (!enabled)
  {
    /* do as you please while the entity is not enabled */
  }
  else
  {
    const uint64_t delta = ddsi_xqos_delta (*e_qos_ptr, newqos, ~(uint64_t)0);
    if (delta == 0)
    {
      /* new settings are identical to the old */
      goto error_or_nochange;
    }
    else if (delta & ~QP_CHANGEABLE_MASK)
    {
      /* not all QoS may be changed according to the spec */
      ret = DDS_RETCODE_IMMUTABLE_POLICY;
      goto error_or_nochange;
    }
    else if (delta & (QP_RXO_MASK | QP_PARTITION))
    {
      /* Cyclone doesn't (yet) support changing QoS that affect matching.  Simply re-doing the
         matching is easy enough, but the consequences are very weird.  E.g., what is the
         expectation if a transient-local writer has published data while its partition QoS is set
         to A, and then changes its partition to B?  Should a reader in B get the data originally
         published in A?

         One can do the same thing with other RxO QoS settings, e.g., the latency budget setting.
         I find that weird, and I'd rather have sane answers to these questions than set up these
         traps and pitfalls for people to walk into ...
       */
      ret = DDS_RETCODE_UNSUPPORTED;
      goto error_or_nochange;
    }
  }

  assert (ret == DDS_RETCODE_OK);
  if ((ret = dds_entity_deriver_set_qos (e, newqos, enabled)) != DDS_RETCODE_OK)
    goto error_or_nochange;
  else
  {
    dds_delete_qos (*e_qos_ptr);
    *e_qos_ptr = newqos;
  }
  return DDS_RETCODE_OK;

error_or_nochange:
  dds_delete_qos (newqos);
  return ret;
}

static dds_return_t dds_set_qos_locked_impl (dds_entity *e, const dds_qos_t *qos, uint64_t mask)
{
  const struct ddsrt_log_cfg *logcfg = &e->m_domain->gv.logconfig;
  dds_entity_kind_t kind = dds_entity_kind (e);
  if (kind != DDS_KIND_TOPIC)
  {
    return dds_set_qos_locked_raw (e, &e->m_qos, qos, mask, logcfg);
  }
  else
  {
    /* Topics must be enabled for now (all are currently, so for now it is not a meaningful limitation):
       there can only be a single QoS (or different versions with the same name can have different QoS -
       in particular a different value for TOPIC_DATA - and therefore the idea that it is a free-for-all
       on the QoS for a disabled entity falls apart for topics.

       FIXME: topic should have a QoS object while still disabled */
    assert (e->m_flags & DDS_ENTITY_ENABLED);
    struct dds_topic * const tp = (struct dds_topic *) e;
    struct dds_participant * const pp = dds_entity_participant (e);
    struct dds_ktopic * const ktp = tp->m_ktopic;
    dds_return_t rc;
    ddsrt_mutex_lock (&pp->m_entity.m_mutex);
    while (ktp->defer_set_qos != 0)
      ddsrt_cond_wait (&pp->m_entity.m_cond, &pp->m_entity.m_mutex);

    rc = dds_set_qos_locked_raw (e, &ktp->qos, qos, mask, logcfg);
    ddsrt_mutex_unlock (&pp->m_entity.m_mutex);
    return rc;
  }
}

static void pushdown_pubsub_qos (dds_entity *e)
{
  /* e claimed but no mutex held */
  struct dds_entity *c;
  dds_instance_handle_t last_iid = 0;
  ddsrt_mutex_lock (&e->m_mutex);
  while ((c = ddsrt_avl_lookup_succ (&dds_entity_children_td, &e->m_children, &last_iid)) != NULL)
  {
    struct dds_entity *x;
    last_iid = c->m_iid;
    if (dds_entity_pin (c->m_hdllink.hdl, &x) == DDS_RETCODE_OK)
    {
      assert (x == c);
      assert (dds_entity_kind (c) == DDS_KIND_READER || dds_entity_kind (c) == DDS_KIND_WRITER);
      /* see dds_get_children for why "c" remains valid despite unlocking m_mutex;
         unlock e, lock c, relock e sequence is to avoid locking a child while holding the parent */
      ddsrt_mutex_unlock (&e->m_mutex);

      ddsrt_mutex_lock (&c->m_mutex);
      ddsrt_mutex_lock (&e->m_mutex);
      dds_set_qos_locked_impl (c, e->m_qos, QP_GROUP_DATA | QP_PARTITION);
      ddsrt_mutex_unlock (&c->m_mutex);
      dds_entity_unpin (c);
    }
  }
  ddsrt_mutex_unlock (&e->m_mutex);
}

static void pushdown_topic_qos (dds_entity *e, struct dds_ktopic *ktp)
{
  /* on input: both entities claimed but no mutexes held */
  enum { NOP, PROP, CHANGE } todo;
  switch (dds_entity_kind (e))
  {
    case DDS_KIND_READER: {
      dds_reader *rd = (dds_reader *) e;
      todo = (rd->m_topic->m_ktopic == ktp) ? CHANGE : NOP;
      break;
    }
    case DDS_KIND_WRITER: {
      dds_writer *wr = (dds_writer *) e;
      todo = (wr->m_topic->m_ktopic == ktp) ? CHANGE : NOP;
      break;
    }
    default: {
      todo = PROP;
      break;
    }
  }
  switch (todo)
  {
    case NOP:
      break;
    case CHANGE: {
      /* may lock topic while holding reader/writer lock */
      struct dds_participant * const pp = dds_entity_participant (e);
      ddsrt_mutex_lock (&e->m_mutex);
      ddsrt_mutex_lock (&pp->m_entity.m_mutex);
      dds_set_qos_locked_impl (e, ktp->qos, QP_TOPIC_DATA);
      ddsrt_mutex_unlock (&pp->m_entity.m_mutex);
      ddsrt_mutex_unlock (&e->m_mutex);
      break;
    }
    case PROP: {
      struct dds_entity *c;
      dds_instance_handle_t last_iid = 0;
      ddsrt_mutex_lock (&e->m_mutex);
      while ((c = ddsrt_avl_lookup_succ (&dds_entity_children_td, &e->m_children, &last_iid)) != NULL)
      {
        struct dds_entity *x;
        last_iid = c->m_iid;
        if (dds_entity_pin (c->m_hdllink.hdl, &x) == DDS_RETCODE_OK)
        {
          assert (x == c);
          /* see dds_get_children for why "c" remains valid despite unlocking m_mutex */
          ddsrt_mutex_unlock (&e->m_mutex);
          pushdown_topic_qos (c, ktp);
          ddsrt_mutex_lock (&e->m_mutex);
          dds_entity_unpin (c);
        }
      }
      ddsrt_mutex_unlock (&e->m_mutex);
      break;
    }
  }
}

dds_return_t dds_set_qos (dds_entity_t entity, const dds_qos_t *qos)
{
  dds_entity *e;
  dds_return_t ret;
  if (qos == NULL)
    return DDS_RETCODE_BAD_PARAMETER;
  if ((ret = dds_entity_pin (entity, &e)) < 0)
    return ret;

  const dds_entity_kind_t kind = dds_entity_kind (e);
  if (!dds_entity_supports_set_qos (e))
  {
    dds_entity_unpin (e);
    return DDS_RETCODE_ILLEGAL_OPERATION;
  }

  ddsrt_mutex_lock (&e->m_mutex);
  ret = dds_set_qos_locked_impl (e, qos, entity_kind_qos_mask (kind));
  ddsrt_mutex_unlock (&e->m_mutex);
  if (ret < 0)
  {
    dds_entity_unpin (e);
    return ret;
  }

  switch (dds_entity_kind (e))
  {
    case DDS_KIND_TOPIC: {
      dds_entity *pp;
      assert (dds_entity_kind (e->m_parent) == DDS_KIND_PARTICIPANT);
      if (dds_entity_pin (e->m_parent->m_hdllink.hdl, &pp) == DDS_RETCODE_OK)
      {
        struct dds_topic *tp = (struct dds_topic *) e;
        pushdown_topic_qos (pp, tp->m_ktopic);
        dds_entity_unpin (pp);
      }
      break;
    }
    case DDS_KIND_PUBLISHER:
    case DDS_KIND_SUBSCRIBER: {
      pushdown_pubsub_qos (e);
      break;
    }
    default: {
      break;
    }
  }

  dds_entity_unpin (e);
  return 0;
}

dds_return_t dds_get_listener (dds_entity_t entity, dds_listener_t *listener)
{
  dds_entity *e;
  dds_return_t ret;
  if (listener == NULL)
    return DDS_RETCODE_BAD_PARAMETER;
  else if ((ret = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return ret;
  else
  {
    ddsrt_mutex_lock (&e->m_observers_lock);
    dds_copy_listener (listener, &e->m_listener);
    ddsrt_mutex_unlock (&e->m_observers_lock);
    dds_entity_unpin (e);
    return DDS_RETCODE_OK;
  }
}

static void clear_status_with_listener (struct dds_entity *e)
{
  const struct dds_listener *lst = &e->m_listener;
  status_mask_t mask = 0;
  if (lst->on_inconsistent_topic)
    mask |= DDS_INCONSISTENT_TOPIC_STATUS;
  if (lst->on_liveliness_lost)
    mask |= DDS_LIVELINESS_LOST_STATUS;
  if (lst->on_offered_deadline_missed)
    mask |= DDS_OFFERED_DEADLINE_MISSED_STATUS;
  if (lst->on_offered_deadline_missed_arg)
    mask |= DDS_OFFERED_DEADLINE_MISSED_STATUS;
  if (lst->on_offered_incompatible_qos)
    mask |= DDS_OFFERED_INCOMPATIBLE_QOS_STATUS;
  if (lst->on_data_on_readers)
    mask |= DDS_DATA_ON_READERS_STATUS;
  if (lst->on_sample_lost)
    mask |= DDS_SAMPLE_LOST_STATUS;
  if (lst->on_data_available)
    mask |= DDS_DATA_AVAILABLE_STATUS;
  if (lst->on_sample_rejected)
    mask |= DDS_SAMPLE_REJECTED_STATUS;
  if (lst->on_liveliness_changed)
    mask |= DDS_LIVELINESS_CHANGED_STATUS;
  if (lst->on_requested_deadline_missed)
    mask |= DDS_REQUESTED_DEADLINE_MISSED_STATUS;
  if (lst->on_requested_incompatible_qos)
    mask |= DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS;
  if (lst->on_publication_matched)
    mask |= DDS_PUBLICATION_MATCHED_STATUS;
  if (lst->on_subscription_matched)
    mask |= DDS_SUBSCRIPTION_MATCHED_STATUS;
  ddsrt_atomic_and32 (&e->m_status.m_status_and_mask, ~(uint32_t)mask);
}

static void pushdown_listener (dds_entity *e)
{
  /* Note: e is claimed, no mutexes held */
  struct dds_entity *c;
  dds_instance_handle_t last_iid = 0;
  ddsrt_mutex_lock (&e->m_mutex);
  while ((c = ddsrt_avl_lookup_succ (&dds_entity_children_td, &e->m_children, &last_iid)) != NULL)
  {
    struct dds_entity *x;
    last_iid = c->m_iid;
    if (dds_entity_pin (c->m_hdllink.hdl, &x) == DDS_RETCODE_OK)
    {
      ddsrt_mutex_unlock (&e->m_mutex);

      ddsrt_mutex_lock (&c->m_observers_lock);
      while (c->m_cb_pending_count > 0)
        ddsrt_cond_wait (&c->m_observers_cond, &c->m_observers_lock);

      ddsrt_mutex_lock (&e->m_observers_lock);
      dds_override_inherited_listener (&c->m_listener, &e->m_listener);
      ddsrt_mutex_unlock (&e->m_observers_lock);

      clear_status_with_listener (c);
      ddsrt_mutex_unlock (&c->m_observers_lock);

      pushdown_listener (c);

      ddsrt_mutex_lock (&e->m_mutex);
      dds_entity_unpin (c);
    }
  }
  ddsrt_mutex_unlock (&e->m_mutex);
}

dds_return_t dds_set_listener (dds_entity_t entity, const dds_listener_t *listener)
{
  dds_entity *e, *x;
  dds_return_t rc;

  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;

  ddsrt_mutex_lock (&e->m_observers_lock);
  while (e->m_cb_pending_count > 0)
    ddsrt_cond_wait (&e->m_observers_cond, &e->m_observers_lock);

  /* new listener is constructed by combining "listener" with the ancestral listeners;
     the new set of listeners is then pushed down into the descendant entities, overriding
     the ones they originally inherited from */
  dds_reset_listener (&e->m_listener);
  if (listener)
    dds_merge_listener (&e->m_listener, listener);
  x = e;
  while (dds_entity_kind (x) != DDS_KIND_CYCLONEDDS)
  {
    x = x->m_parent;
    ddsrt_mutex_lock (&x->m_observers_lock);
    dds_inherit_listener (&e->m_listener, &x->m_listener);
    ddsrt_mutex_unlock (&x->m_observers_lock);
  }
  clear_status_with_listener (e);
  ddsrt_mutex_unlock (&e->m_observers_lock);
  pushdown_listener (e);
  dds_entity_unpin (e);
  return DDS_RETCODE_OK;
}

dds_return_t dds_enable (dds_entity_t entity)
{
  dds_entity *e;
  dds_return_t rc;

  if ((rc = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return rc;

  if ((e->m_flags & DDS_ENTITY_ENABLED) == 0)
  {
    /* TODO: Really enable. */
    e->m_flags |= DDS_ENTITY_ENABLED;
    DDS_CERROR (&e->m_domain->gv.logconfig, "Delayed entity enabling is not supported\n");
  }
  dds_entity_unlock (e);
  return DDS_RETCODE_OK;
}

dds_return_t dds_get_status_changes (dds_entity_t entity, uint32_t *status)
{
  dds_entity *e;
  dds_return_t ret;

  if (status == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return ret;

  if (!dds_entity_supports_validate_status (e))
    ret = DDS_RETCODE_ILLEGAL_OPERATION;
  else
  {
    assert (entity_has_status (e));
    *status = ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask) & SAM_STATUS_MASK;
    ret = DDS_RETCODE_OK;
  }
  dds_entity_unlock(e);
  return ret;
}

dds_return_t dds_get_status_mask (dds_entity_t entity, uint32_t *mask)
{
  dds_entity *e;
  dds_return_t ret;

  if (mask == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return ret;

  if (!dds_entity_supports_validate_status (e))
    ret = DDS_RETCODE_ILLEGAL_OPERATION;
  else
  {
    assert (entity_has_status (e));
    *mask = ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask) >> SAM_ENABLED_SHIFT;
    ret = DDS_RETCODE_OK;
  }
  dds_entity_unpin(e);
  return ret;
}

dds_return_t dds_get_enabled_status (dds_entity_t entity, uint32_t *status)
{
  return dds_get_status_mask(entity, status);
}

dds_return_t dds_set_status_mask (dds_entity_t entity, uint32_t mask)
{
  dds_entity *e;
  dds_return_t ret;

  if ((mask & ~SAM_STATUS_MASK) != 0)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return ret;

  if (dds_handle_is_closed (&e->m_hdllink))
  {
    /* The sole reason for locking the mutex was so we can do this atomically with
       respect to dds_delete (which in turn requires checking whether the handle
       is still open), because delete relies on the mask to shut down all listener
       invocations.  */
    dds_entity_unlock (e);
    return DDS_RETCODE_PRECONDITION_NOT_MET;
  }

  if ((ret = dds_entity_deriver_validate_status (e, mask)) == DDS_RETCODE_OK)
  {
    assert (entity_has_status (e));
    ddsrt_mutex_lock (&e->m_observers_lock);
    while (e->m_cb_pending_count > 0)
      ddsrt_cond_wait (&e->m_observers_cond, &e->m_observers_lock);

    uint32_t old, new;
    do {
      old = ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask);
      new = (mask << SAM_ENABLED_SHIFT) | (old & mask);
    } while (!ddsrt_atomic_cas32 (&e->m_status.m_status_and_mask, old, new));
    ddsrt_mutex_unlock (&e->m_observers_lock);
  }
  dds_entity_unlock (e);
  return ret;
}

dds_return_t dds_set_enabled_status(dds_entity_t entity, uint32_t mask)
{
  return dds_set_status_mask (entity, mask);
}

static dds_return_t dds_readtake_status (dds_entity_t entity, uint32_t *status, uint32_t mask, bool reset)
{
  dds_entity *e;
  dds_return_t ret;

  if (status == NULL)
    return DDS_RETCODE_BAD_PARAMETER;
  if ((mask & ~SAM_STATUS_MASK) != 0)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return ret;

  if ((ret = dds_entity_deriver_validate_status (e, mask)) == DDS_RETCODE_OK)
  {
    uint32_t s;
    assert (entity_has_status (e));
    if (reset)
      s = ddsrt_atomic_and32_ov (&e->m_status.m_status_and_mask, ~mask) & mask;
    else
      s = ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask) & mask;
    *status = s;
  }
  dds_entity_unlock (e);
  return ret;
}

dds_return_t dds_read_status (dds_entity_t entity, uint32_t *status, uint32_t mask)
{
  return dds_readtake_status (entity, status, mask, false);
}

dds_return_t dds_take_status (dds_entity_t entity, uint32_t *status, uint32_t mask)
{
  return dds_readtake_status (entity, status, mask, true);
}

dds_return_t dds_get_domainid (dds_entity_t entity, dds_domainid_t *id)
{
  dds_entity *e;
  dds_return_t rc;

  if (id == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;

  *id = e->m_domain ? e->m_domain->m_id : DDS_DOMAIN_DEFAULT;
  dds_entity_unpin (e);
  return DDS_RETCODE_OK;
}

dds_return_t dds_get_instance_handle (dds_entity_t entity, dds_instance_handle_t *ihdl)
{
  dds_entity *e;
  dds_return_t ret;

  if (ihdl == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return ret;
  *ihdl = e->m_iid;
  dds_entity_unpin(e);
  return ret;
}

dds_return_t dds_get_guid (dds_entity_t entity, dds_guid_t *guid)
{
  dds_entity *e;
  dds_return_t ret;

  if (guid == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return ret;
  switch (dds_entity_kind (e))
  {
    case DDS_KIND_PARTICIPANT:
    case DDS_KIND_READER:
    case DDS_KIND_WRITER: {
      DDSRT_STATIC_ASSERT (sizeof (dds_guid_t) == sizeof (ddsi_guid_t));
      ddsi_guid_t tmp = nn_ntoh_guid (e->m_guid);
      memcpy (guid, &tmp, sizeof (*guid));
      ret = DDS_RETCODE_OK;
      break;
    }
    default: {
      ret = DDS_RETCODE_ILLEGAL_OPERATION;
      break;
    }
  }
  dds_entity_unpin(e);
  return ret;
}

dds_return_t dds_entity_pin (dds_entity_t hdl, dds_entity **eptr)
{
  dds_return_t hres;
  struct dds_handle_link *hdllink;
  if ((hres = dds_handle_pin (hdl, &hdllink)) < 0)
    return hres;
  else
  {
    *eptr = dds_entity_from_handle_link (hdllink);
    return DDS_RETCODE_OK;
  }
}

dds_return_t dds_entity_pin_for_delete (dds_entity_t hdl, bool explicit, dds_entity **eptr)
{
  dds_return_t hres;
  struct dds_handle_link *hdllink;
  if ((hres = dds_handle_pin_for_delete (hdl, explicit, &hdllink)) < 0)
    return hres;
  else
  {
    *eptr = dds_entity_from_handle_link (hdllink);
    return DDS_RETCODE_OK;
  }
}

void dds_entity_unpin (dds_entity *e)
{
  dds_handle_unpin (&e->m_hdllink);
}

dds_return_t dds_entity_lock (dds_entity_t hdl, dds_entity_kind_t kind, dds_entity **eptr)
{
  dds_return_t hres;
  dds_entity *e;
  if ((hres = dds_entity_pin (hdl, &e)) < 0)
    return hres;
  else
  {
    if (dds_entity_kind (e) != kind && kind != DDS_KIND_DONTCARE)
    {
      dds_entity_unpin (e);
      return DDS_RETCODE_ILLEGAL_OPERATION;
    }

    ddsrt_mutex_lock (&e->m_mutex);
    *eptr = e;
    return DDS_RETCODE_OK;
  }
}

void dds_entity_unlock (dds_entity *e)
{
  ddsrt_mutex_unlock (&e->m_mutex);
  dds_entity_unpin (e);
}

dds_return_t dds_triggered (dds_entity_t entity)
{
  dds_entity *e;
  dds_return_t ret;

  if ((ret = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return ret;
  if (entity_has_status (e))
    ret = ((ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask) & SAM_STATUS_MASK) != 0);
  else
    ret = (ddsrt_atomic_ld32 (&e->m_status.m_trigger) != 0);
  dds_entity_unlock (e);
  return ret;
}

static bool in_observer_list_p (const struct dds_entity *observed, const dds_waitset *observer)
{
  dds_entity_observer *cur;
  for (cur = observed->m_observers; cur != NULL; cur = cur->m_next)
    if (cur->m_observer == observer)
      return true;
  return false;
}

dds_return_t dds_entity_observer_register (dds_entity *observed, dds_waitset *observer, dds_entity_callback_t cb, dds_entity_attach_callback_t attach_cb, void *attach_arg, dds_entity_delete_callback_t delete_cb)
{
  dds_return_t rc;
  assert (observed);
  ddsrt_mutex_lock (&observed->m_observers_lock);
  if (in_observer_list_p (observed, observer))
    rc = DDS_RETCODE_PRECONDITION_NOT_MET;
  else if (!attach_cb (observer, observed, attach_arg))
    rc = DDS_RETCODE_BAD_PARAMETER;
  else
  {
    dds_entity_observer *o = ddsrt_malloc (sizeof (dds_entity_observer));
    o->m_cb = cb;
    o->m_delete_cb = delete_cb;
    o->m_observer = observer;
    o->m_next = observed->m_observers;
    observed->m_observers = o;
    rc = DDS_RETCODE_OK;
  }
  ddsrt_mutex_unlock (&observed->m_observers_lock);
  return rc;
}

dds_return_t dds_entity_observer_unregister (dds_entity *observed, dds_waitset *observer, bool invoke_delete_cb)
{
  dds_return_t rc;
  dds_entity_observer *prev, *idx;

  ddsrt_mutex_lock (&observed->m_observers_lock);
  prev = NULL;
  idx = observed->m_observers;
  while (idx != NULL && idx->m_observer != observer)
  {
    prev = idx;
    idx = idx->m_next;
  }
  if (idx == NULL)
    rc = DDS_RETCODE_PRECONDITION_NOT_MET;
  else
  {
    if (prev == NULL)
      observed->m_observers = idx->m_next;
    else
      prev->m_next = idx->m_next;
    if (invoke_delete_cb)
      idx->m_delete_cb (idx->m_observer, observed->m_hdllink.hdl);
    ddsrt_free (idx);
    rc = DDS_RETCODE_OK;
  }
  ddsrt_mutex_unlock (&observed->m_observers_lock);
  return rc;
}

static void dds_entity_observers_signal (dds_entity *observed, uint32_t status)
{
  for (dds_entity_observer *idx = observed->m_observers; idx; idx = idx->m_next)
    idx->m_cb (idx->m_observer, observed->m_hdllink.hdl, status);
}

static void dds_entity_observers_signal_delete (dds_entity *observed)
{
  dds_entity_observer *idx;
  idx = observed->m_observers;
  while (idx != NULL)
  {
    dds_entity_observer *next = idx->m_next;
    idx->m_delete_cb (idx->m_observer, observed->m_hdllink.hdl);
    ddsrt_free (idx);
    idx = next;
  }
  observed->m_observers = NULL;
}

void dds_entity_status_signal (dds_entity *e, uint32_t status)
{
  ddsrt_mutex_lock (&e->m_observers_lock);
  dds_entity_observers_signal (e, status);
  ddsrt_mutex_unlock (&e->m_observers_lock);
}

void dds_entity_status_set (dds_entity *e, status_mask_t status)
{
  assert (entity_has_status (e));
  uint32_t old, delta, new;
  do {
    old = ddsrt_atomic_ld32 (&e->m_status.m_status_and_mask);
    delta = ((uint32_t) status & (old >> SAM_ENABLED_SHIFT));
    if (delta == 0)
      return;
    new = old | delta;
  } while (!ddsrt_atomic_cas32 (&e->m_status.m_status_and_mask, old, new));
  if (delta)
    dds_entity_observers_signal (e, status);
}

void dds_entity_trigger_set (dds_entity *e, uint32_t t)
{
  assert (! entity_has_status (e));
  uint32_t oldst;
  ddsrt_mutex_lock (&e->m_observers_lock);
  do {
    oldst = ddsrt_atomic_ld32 (&e->m_status.m_trigger);
  } while (!ddsrt_atomic_cas32 (&e->m_status.m_trigger, oldst, t));
  if (oldst == 0 && t != 0)
    dds_entity_observers_signal (e, t);
  ddsrt_mutex_unlock (&e->m_observers_lock);
}

dds_entity_t dds_get_topic (dds_entity_t entity)
{
  dds_return_t rc;
  dds_entity_t hdl;
  dds_entity *e;

  if ((rc = dds_entity_lock (entity, DDS_KIND_DONTCARE, &e)) != DDS_RETCODE_OK)
    return rc;
  switch (dds_entity_kind (e))
  {
    case DDS_KIND_READER: {
      dds_reader *rd = (dds_reader *) e;
      hdl = rd->m_topic->m_entity.m_hdllink.hdl;
      break;
    }
    case DDS_KIND_WRITER: {
      dds_writer *wr = (dds_writer *) e;
      hdl = wr->m_topic->m_entity.m_hdllink.hdl;
      break;
    }
    case DDS_KIND_COND_READ:
    case DDS_KIND_COND_QUERY: {
      assert (dds_entity_kind (e->m_parent) == DDS_KIND_READER);
      dds_reader *rd = (dds_reader *) e->m_parent;
      hdl = rd->m_topic->m_entity.m_hdllink.hdl;
      break;
    }
    default: {
      hdl = DDS_RETCODE_ILLEGAL_OPERATION;
      break;
    }
  }
  dds_entity_unlock (e);
  return hdl;
}

dds_return_t dds_generic_unimplemented_operation_manykinds (dds_entity_t handle, size_t nkinds, const dds_entity_kind_t *kinds)
{
  dds_entity *e;
  dds_return_t ret;
  if ((ret = dds_entity_pin (handle, &e)) != DDS_RETCODE_OK)
    return ret;
  else
  {
    const dds_entity_kind_t actual = dds_entity_kind (e);
    ret = DDS_RETCODE_ILLEGAL_OPERATION;
    for (size_t i = 0; i < nkinds; i++)
    {
      if (kinds[i] == actual)
      {
        /* If the handle happens to be for an entity of the right kind, return unsupported */
        ret = DDS_RETCODE_UNSUPPORTED;
        break;
      }
    }
    dds_entity_unpin (e);
    return ret;
  }
}

dds_return_t dds_generic_unimplemented_operation (dds_entity_t handle, dds_entity_kind_t kind)
{
  return dds_generic_unimplemented_operation_manykinds (handle, 1, &kind);
}

dds_return_t dds_assert_liveliness (dds_entity_t entity)
{
  dds_return_t rc;
  dds_entity *e, *ewr;

  if ((rc = dds_entity_pin (entity, &e)) != DDS_RETCODE_OK)
    return rc;
  switch (dds_entity_kind (e))
  {
    case DDS_KIND_PARTICIPANT: {
      write_pmd_message_guid (&e->m_domain->gv, &e->m_guid, PARTICIPANT_MESSAGE_DATA_KIND_MANUAL_LIVELINESS_UPDATE);
      break;
    }
    case DDS_KIND_WRITER: {
      if ((rc = dds_entity_lock (entity, DDS_KIND_WRITER, &ewr)) != DDS_RETCODE_OK)
        return rc;
      if ((rc = write_hb_liveliness (&e->m_domain->gv, &e->m_guid, ((struct dds_writer *)ewr)->m_xp)) != DDS_RETCODE_OK)
        return rc;
      dds_entity_unlock (e);
      break;
    }
    default: {
      rc = DDS_RETCODE_ILLEGAL_OPERATION;
      break;
    }
  }
  dds_entity_unpin (e);
  return rc;
}
