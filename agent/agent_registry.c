/*
 * agent_registry.c
 *
 * Maintain a registry of MIB subtrees, together
 *   with related information regarding mibmodule, sessions, etc
 */

#define IN_SNMP_VARS_C

#include <config.h>
#if HAVE_STRING_H
#include <string.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#if HAVE_WINSOCK_H
#include <winsock.h>
#endif
#if TIME_WITH_SYS_TIME
# ifdef WIN32
#  include <sys/timeb.h>
# else
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include "mibincl.h"
#include "default_store.h"
#include "ds_agent.h"
#include "callback.h"
#include "agent_callbacks.h"
#include "agent_registry.h"

#include "snmpd.h"
#include "mibgroup/struct.h"
#include "mib_module_includes.h"

#ifdef USING_AGENTX_SUBAGENT_MODULE
#include "agentx/subagent.h"
#include "agentx/client.h"
#endif


struct snmp_index {
    struct variable_list	varbind;	/* or pointer to var_list ? */
    struct snmp_session		*session;	/* NULL implies unused  ? */
    struct snmp_index		*next_oid;
    struct snmp_index		*prev_oid;
    struct snmp_index		*next_idx;
} *snmp_index_head = NULL; 
struct subtree *subtrees;

int tree_compare(const struct subtree *ap, const struct subtree *bp)
{
  return snmp_oid_compare(ap->name,ap->namelen,bp->name,bp->namelen);
}



	/*
	 *  Split the subtree into two at the specified point,
	 *    returning the new (second) subtree
	 */
struct subtree *
split_subtree(struct subtree *current, oid name[], int name_len )
{
    struct subtree *new_sub, *ptr;
    int i;
    char *cp;

    if ( snmp_oid_compare(name, name_len,
			  current->end, current->end_len) > 0 )
	return NULL;	/* Split comes after the end of this subtree */

    new_sub = (struct subtree *)malloc(sizeof(struct subtree));
    if ( new_sub == NULL )
	return NULL;
    memcpy(new_sub, current, sizeof(struct subtree));

	/* Set up the point of division */
    memcpy(current->end,   name, name_len*sizeof(oid));
    memcpy(new_sub->start, name, name_len*sizeof(oid));
    current->end_len   = name_len;
    new_sub->start_len = name_len;

	/*
	 * Split the variables between the two new subtrees
	 */
    i = current->variables_len;
    current->variables_len = 0;

    for ( ; i > 0 ; i-- ) {
		/* Note that the variable "name" field omits
		   the prefix common to the whole registration,
		   hence the strange comparison here */
	if ( snmp_oid_compare( new_sub->variables[0].name,
			       new_sub->variables[0].namelen,
			       name     + current->namelen, 
			       name_len - current->namelen ) >= 0 )
	    break;	/* All following variables belong to the second subtree */

	current->variables_len++;
	new_sub->variables_len--;
	cp = (char *)new_sub->variables;
	new_sub->variables = (struct variable *)(cp + new_sub->variables_width);
    }

	/* Propogate this split down through any children */
    if ( current->children )
	new_sub->children = split_subtree(current->children, name, name_len);

	/* Retain the correct linking of the list */
    for ( ptr = current ; ptr != NULL ; ptr=ptr->children )
          ptr->next = new_sub;
    for ( ptr = new_sub ; ptr != NULL ; ptr=ptr->children )
          ptr->prev = current;
    for ( ptr = new_sub->next ; ptr != NULL ; ptr=ptr->children )
          ptr->prev = new_sub;

    return new_sub;
}

int
load_subtree( struct subtree *new_sub )
{
    struct subtree *tree1, *tree2, *new2;
    struct subtree *prev, *next;
    int res;

    if ( new_sub == NULL )
	return MIB_REGISTERED_OK;	/* Degenerate case */

		/*
		 * Find the subtree that contains the start of 
		 *  the new subtree (if any)...
		 */
    tree1 = find_subtree( new_sub->start, new_sub->start_len, NULL );
		/*
		 * ...and the subtree that follows the new one
		 *	(NULL implies this is the final region covered)
		 */  
    if ( tree1 == NULL )
        tree2 = find_subtree_next( new_sub->start, new_sub->start_len, NULL );
    else
	tree2 = tree1->next;


	/*
	 * Handle new subtrees that start in virgin territory.
	 */
    if ( tree1 == NULL ) {
	new2 = NULL;
		/* Is there any overlap with later subtrees ? */
	if ( tree2 && snmp_oid_compare( new_sub->end, new_sub->end_len,
					tree2->start, tree2->start_len ) > 0 )
	    new2 = split_subtree( new_sub, tree2->start, tree2->start_len );

		/*
		 * Link the new subtree (less any overlapping region)
		 *  with the list of existing registrations
		 */
	if ( tree2 ) {
	    new_sub->prev = tree2->prev;
	    tree2->prev       = new_sub;
	}
	else
	    new_sub->prev = find_subtree_previous( new_sub->start, new_sub->start_len, NULL );

	if ( new_sub->prev )
	    new_sub->prev->next = new_sub;
	else
	    subtrees = new_sub;

	new_sub->next     = tree2;

		/*
		 * If there was any overlap,
		 *  recurse to merge in the overlapping region
		 *  (including anything that may follow the overlap)
		 */
	if ( new2 )
	    return load_subtree( new2 );
    }

    else {
	/*
	 *  If the new subtree starts *within* an existing registration
	 *    (rather than at the same point as it), then split the
	 *    existing subtree at this point.
	 */
	if ( snmp_oid_compare( new_sub->start, new_sub->start_len, 
			       tree1->start,   tree1->start_len) != 0 )
	    tree1 = split_subtree( tree1, new_sub->start, new_sub->start_len);
	    if ( tree1 == NULL )
		return MIB_REGISTRATION_FAILED;

	/*  Now consider the end of this existing subtree:
	 *	If it matches the new subtree precisely,
	 *	  simply merge the new one into the list of children
	 *	If it includes the whole of the new subtree,
	 *	  split it at the appropriate point, and merge again
	 *
	 *	If the new subtree extends beyond this existing region,
	 *	  split it, and recurse to merge the two parts.
	 */

	 switch ( snmp_oid_compare( new_sub->end, new_sub->end_len, 
				    tree1->end,   tree1->end_len))  {

		case -1:	/* Existing subtree contains new one */
			(void) split_subtree( tree1,
					new_sub->end, new_sub->end_len);
			/* Fall Through */

		case  0:	/* The two trees match precisely */
			/*
			 * Note: This is the only point where the original
			 *	 registration OID ("name") is used
			 */
			prev = NULL;
			next = tree1;
			while ( next && next->namelen > new_sub->namelen ) {
				prev = next;
				next = next->children;
			}
			while ( next && next->namelen == new_sub->namelen &&
					next->priority < new_sub->priority ) {
				prev = next;
				next = next->children;
			}
			if ( next &&	next->namelen  == new_sub->namelen &&
					next->priority == new_sub->priority )
			   return MIB_DUPLICATE_REGISTRATION;

			if ( prev ) {
			    new_sub->children = next;
			    prev->children    = new_sub;
			    new_sub->prev = prev->prev;
			    new_sub->next = prev->next;
			}
			else {
			    new_sub->children = next;
			    new_sub->prev = next->prev;
			    new_sub->next = next->next;

			    for ( next = new_sub->next ;
			    	  next != NULL ;
				  next = next->children )
					next->prev = new_sub;

			    for ( prev = new_sub->prev ;
			    	  prev != NULL ;
				  prev = prev->children )
					prev->next = new_sub;
			}
			break;

		case  1:	/* New subtree contains the existing one */
	    		new2 = split_subtree( new_sub,
					tree1->end, tree1->end_len);
			res = load_subtree( new_sub );
			if ( res != MIB_REGISTERED_OK )
			    return res;
			return load_subtree( new2 );

	 }

    }
    return 0;
}


int
register_mib_range(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen,
	     int priority,
	     int range_subid,
	     oid range_ubound,
	     struct snmp_session *ss)
{
  struct subtree *subtree, *sub2;
  char c_oid[SPRINT_MAX_LEN];
  int res, i;
  struct register_parameters reg_parms;
  
  subtree = (struct subtree *) malloc(sizeof(struct subtree));
  if ( subtree == NULL )
    return MIB_REGISTRATION_FAILED;
  memset(subtree, 0, sizeof(struct subtree));

  sprint_objid(c_oid, mibloc, mibloclen);
  DEBUGMSGTL(("register_mib", "registering \"%s\" at %s\n",
              moduleName, c_oid));
    
	/*
	 * Create the new subtree node being registered
	 */
  memcpy(subtree->name, mibloc, mibloclen*sizeof(oid));
  subtree->namelen = (u_char) mibloclen;
  memcpy(subtree->start, mibloc, mibloclen*sizeof(oid));
  subtree->start_len = (u_char) mibloclen;
  memcpy(subtree->end, mibloc, mibloclen*sizeof(oid));
  subtree->end[ mibloclen-1 ]++;	/* XXX - or use 'variables' info ? */
  subtree->end_len = (u_char) mibloclen;
  memcpy(subtree->label, moduleName, strlen(moduleName)+1);
  if ( var ) {
    subtree->variables = (struct variable *) malloc(varsize*numvars);
    memcpy(subtree->variables, var, numvars*varsize);
    subtree->variables_len = numvars;
    subtree->variables_width = varsize;
  }
  subtree->priority = priority;
  subtree->session = ss;
  res = load_subtree(subtree);

	/*
	 * If registering a range,
	 *   use the first subtree as a template
	 *   for the rest of the range
	 */
  if (( res == MIB_REGISTERED_OK ) && ( range_subid != 0 )) {
    for ( i = mibloc[range_subid-1] +1 ; i < (int)range_ubound ; i++ ) {
	sub2 = (struct subtree *) malloc(sizeof(struct subtree));
	if ( sub2 == NULL ) {
	    unregister_mib_range( mibloc, mibloclen, priority,
				  range_subid, range_ubound);
	    return MIB_REGISTRATION_FAILED;
	}
	memcpy( sub2, subtree, sizeof(struct subtree));
	sub2->start[range_subid-1] = i;
	sub2->end[  range_subid-1] = i;		/* XXX - ???? */
	res = load_subtree(sub2);
	if ( res != MIB_REGISTERED_OK ) {
	    unregister_mib_range( mibloc, mibloclen, priority,
				  range_subid, range_ubound);
	    return MIB_REGISTRATION_FAILED;
	}
    }
  }


  reg_parms.name = mibloc;
  reg_parms.namelen = mibloclen;
  reg_parms.priority = priority;
  reg_parms.range_subid  = range_subid;
  reg_parms.range_ubound = range_ubound;
  snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_REGISTER_OID,
                      &reg_parms);

  return res;
}

int
register_mib_priority(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen,
	     int priority)
{
  return register_mib_range( moduleName, var, varsize, numvars,
				mibloc, mibloclen, priority, 0, 0, NULL );
}

int
register_mib(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen)
{
  return register_mib_priority( moduleName, var, varsize, numvars,
				mibloc, mibloclen, DEFAULT_MIB_PRIORITY );
}


void
unload_subtree( struct subtree *sub, struct subtree *prev)
{
    struct subtree *ptr;

    if ( prev != NULL ) {	/* non-leading entries are easy */
	prev->children = sub->children;
	return;
    }
			/* otherwise, we need to amend our neighbours as well */

    if ( sub->children == NULL) {	/* just remove this node completely */
	for (ptr = sub->prev ; ptr ; ptr=ptr->children )
	    ptr->next = sub->next;
	for (ptr = sub->next ; ptr ; ptr=ptr->children )
	    ptr->prev = sub->prev;
	return;
    }
    else {
	for (ptr = sub->prev ; ptr ; ptr=ptr->children )
	    ptr->next = sub->children;
	for (ptr = sub->next ; ptr ; ptr=ptr->children )
	    ptr->prev = sub->children;
	return;
    }
}

int
unregister_mib_range( oid *name, size_t len, int priority,
	     		int range_subid, oid range_ubound)
{
  struct subtree *list, *myptr;
  struct subtree *prev, *child;             /* loop through children */
  struct register_parameters reg_parms;

  list = find_subtree( name, len, subtrees );
  if ( list == NULL )
	return MIB_NO_SUCH_REGISTRATION;

  for ( child=list, prev=NULL;  child != NULL;
			 	prev=child, child=child->children ) {
      if (( snmp_oid_compare( child->name, child->namelen, name, len) == 0 )
	  && ( child->priority == priority ))
		break;	/* found it */
  }
  if ( child == NULL )
	return MIB_NO_SUCH_REGISTRATION;

  unload_subtree( child, prev );
  myptr = child;	/* remember this for later */

		/*
		 *  Now handle any occurances in the following subtrees,
		 *	as a result of splitting this range.  Due to the
		 *	nature of the way such splits work, the first
		 * 	subtree 'slice' that doesn't refer to the given
		 *	name marks the end of the original region.
		 *
		 *  This should also serve to register ranges.
		 */

  for ( list = myptr->next ; list != NULL ; list=list->next ) {
  	for ( child=list, prev=NULL;  child != NULL;
			 	      prev=child, child=child->children ) {
	    if (( snmp_oid_compare( child->name, child->namelen,
							name, len) == 0 )
		&& ( child->priority == priority )) {

		    unload_subtree( child, prev );
		    free_subtree( child );
		    break;
	    }
	}
	if ( child == NULL )	/* Didn't find the given name */
	    break;
  }
  free_subtree( myptr );
  
  reg_parms.name = name;
  reg_parms.namelen = len;
  reg_parms.priority = priority;
  reg_parms.range_subid  = range_subid;
  reg_parms.range_ubound = range_ubound;
  snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_UNREGISTER_OID,
                      &reg_parms);

  return MIB_UNREGISTERED_OK;
}

int
unregister_mib_priority(oid *name, size_t len, int priority)
{
  return unregister_mib_range( name, len, priority, 0, 0 );
}

int
unregister_mib(oid *name,
	       size_t len)
{
  return unregister_mib_priority( name, len, DEFAULT_MIB_PRIORITY );
}

void
unregister_mibs_by_session (struct snmp_session *ss)
{
  struct subtree *list, *list2;
  struct subtree *child, *prev, *next_child;

  for( list = subtrees; list != NULL; list = list2) {
    list2 = list->next;
    for ( child=list, prev=NULL;  child != NULL; child=next_child ) {

      next_child = child->children;
      if (( (ss->flags & SNMP_FLAGS_SUBSESSION) && child->session == ss ) ||
          (!(ss->flags & SNMP_FLAGS_SUBSESSION) &&
                                      child->session->subsession == ss )) {
              unload_subtree( child, prev );
              free_subtree( child );
      }
      else
          prev = child;
    }
  }
}


struct subtree *
free_subtree(struct subtree *st)
{
  struct subtree *ret = NULL;
  if ((snmp_oid_compare(st->name, st->namelen, st->start, st->start_len) == 0)
       && (st->variables != NULL))
    free(st->variables);
  if (st->next != NULL)
    ret = st->next;
  free(st);
  return ret;
}

/* in_a_view: determines if a given snmp_pdu is allowed to see a
   given name/namelen OID pointer
   name         IN - name of var, OUT - name matched
   nameLen      IN -number of sub-ids in name, OUT - subid-is in matched name
   pi           IN - relevant auth info re PDU 
   cvp          IN - relevant auth info re mib module
*/

int
in_a_view(oid		  *name,      /* IN - name of var, OUT - name matched */
          size_t	  *namelen,   /* IN -number of sub-ids in name*/
          struct snmp_pdu *pdu,       /* IN - relevant auth info re PDU */
          int	           type)      /* IN - variable type being checked */
{

  struct view_parameters view_parms;
  view_parms.pdu = pdu;
  view_parms.name = name;
  view_parms.namelen = *namelen;
  view_parms.errorcode = 1;

  if (pdu->flags & UCD_MSG_FLAG_ALWAYS_IN_VIEW)
    return 1;		/* Enable bypassing of view-based access control */

  /* check for v1 and counter64s, since snmpv1 doesn't support it */
  if (pdu->version == SNMP_VERSION_1 && type == ASN_COUNTER64)
    return 0;
  switch (pdu->version) {
  case SNMP_VERSION_1:
  case SNMP_VERSION_2c:
  case SNMP_VERSION_3:
    snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_ACM_CHECK,
                        &view_parms);
    return view_parms.errorcode;
  }
  return 0;
}



int
compare_tree(oid *name1,
	     size_t len1, 
	     oid *name2, 
	     size_t len2)
{
    register int    len;

    /* len = minimum of len1 and len2 */
    if (len1 < len2)
	len = len1;
    else
	len = len2;
    /* find first non-matching byte */
    while(len-- > 0){
	if (*name1 < *name2)
	    return -1;
	if (*name2++ < *name1++)
	    return 1;
    }
    /* bytes match up to length of shorter string */
    if (len1 < len2)
	return -1;  /* name1 shorter, so it is "less" */
    /* name1 matches name2 for length of name2, or they are equal */
    return 0;
}

struct subtree *find_subtree_previous(oid *name,
			     size_t len,
			     struct subtree *subtree)
{
  struct subtree *myptr, *previous = NULL;

  if ( subtree )
	myptr = subtree;
  else
	myptr = subtrees;	/* look through everything */

  for( ; myptr != NULL; previous = myptr, myptr = myptr->next) {
    if (snmp_oid_compare(name, len, myptr->start, myptr->start_len) < 0)
      return previous;
  }
  return previous;
}

struct subtree *find_subtree_next(oid *name, 
				  size_t len,
				  struct subtree *subtree)
{
  struct subtree *myptr = NULL;

  myptr = find_subtree_previous(name, len, subtree);
  if ( myptr != NULL ) {
     myptr = myptr->next;
     while ( myptr && (myptr->variables == NULL || myptr->variables_len == 0) )
         myptr = myptr->next;
     return myptr;
  }
  else if (subtree && snmp_oid_compare(name, len, subtree->start, subtree->start_len) < 0)
     return subtree;
  else
     return NULL;
}

struct subtree *find_subtree(oid *name,
			     size_t len,
			     struct subtree *subtree)
{
  struct subtree *myptr;

  myptr = find_subtree_previous(name, len, subtree);
  if (myptr && snmp_oid_compare(name, len, myptr->end, myptr->end_len) < 0)
	return myptr;

  return NULL;
}

struct snmp_session *get_session_for_oid( oid *name, size_t len)
{
   struct subtree *myptr;

   myptr = find_subtree_previous(name, len, subtrees);
   while ( myptr && myptr->variables == NULL )
        myptr = myptr->next;

   if ( myptr == NULL )
        return NULL;
   else
        return myptr->session;
}



static struct subtree root_subtrees[] = {
   { { 0 }, 1 },	/* ccitt */
   { { 1 }, 1 },	/*  iso  */
   { { 2 }, 1 }		/* joint-ccitt-iso */
};


void setup_tree (void)
{
#ifdef USING_AGENTX_SUBAGENT_MODULE
  int role;

  role = ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE);
  ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, MASTER_AGENT);
#endif

  register_mib("", NULL, 0, 0,
	root_subtrees[0].name,  root_subtrees[0].namelen);
  register_mib("", NULL, 0, 0,
	root_subtrees[1].name,  root_subtrees[1].namelen);
  register_mib("", NULL, 0, 0,
	root_subtrees[2].name,  root_subtrees[2].namelen);

  /* Support for 'static' subtrees (subtrees_old) has now been dropped */

  /* No longer necessary to sort the mib tree - this is inherent in
     the construction of the subtree structure */

#ifdef USING_AGENTX_SUBAGENT_MODULE
  ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, role);
#endif
}

	/*
	 * Initial support for index allocation
	 *
	 *  N.B:  The final 'NULL' parameter in the register_index()
	 *	calls below should be replaced by the 'main' session pointer.
	 *
	 *	  These routines (or the main 'register_index()' routine)
	 *	should check for subagent role, and pass the request off
	 *	to the master.
	 */
struct variable_list *
register_string_index( oid *name, size_t name_len, char *cp )
{
    struct variable_list varbind;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_OCTET_STR;
    snmp_set_var_objid( &varbind, name, name_len );
    if ( cp != ANY_STRING_INDEX ) {
        snmp_set_var_value( &varbind, cp, strlen(cp) );
	return( register_index( &varbind, ALLOCATE_THIS_INDEX, NULL ));
    }
    else
	return( register_index( &varbind, ALLOCATE_ANY_INDEX, NULL ));
}

struct variable_list *
register_int_index( oid *name, size_t name_len, int val )
{
    struct variable_list varbind;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_INTEGER;
    snmp_set_var_objid( &varbind, name, name_len );
    varbind.val.string = varbind.buf;
    if ( val != ANY_INTEGER_INDEX ) {
        varbind.val_len = sizeof(long);
        *varbind.val.integer = val;
	return( register_index( &varbind, ALLOCATE_THIS_INDEX, NULL ));
    }
    else
	return( register_index( &varbind, ALLOCATE_ANY_INDEX, NULL ));
}

struct variable_list *
register_oid_index( oid *name, size_t name_len,
		    oid *value, size_t value_len )
{
    struct variable_list varbind;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_OBJECT_ID;
    snmp_set_var_objid( &varbind, name, name_len );
    if ( value != ANY_OID_INDEX ) {
        snmp_set_var_value( &varbind, value, value_len*sizeof(oid) );
	return( register_index( &varbind, ALLOCATE_THIS_INDEX, NULL ));
    }
    else
	return( register_index( &varbind, ALLOCATE_ANY_INDEX, NULL ));
}

struct variable_list*
register_index(struct variable_list *varbind, int flags, struct snmp_session *ss )
{
    struct snmp_index *new_index, *idxptr, *idxptr2;
    struct snmp_index *prev_oid_ptr, *prev_idx_ptr;
    int res, res2, i;

#if defined(USING_AGENTX_SUBAGENT_MODULE) && !defined(TESTING)
    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE) == SUB_AGENT )
	return( agentx_register_index( ss, varbind, flags ));
#endif
		/* Look for the requested OID entry */
    prev_oid_ptr = NULL;
    prev_idx_ptr = NULL;
    for( idxptr = snmp_index_head ; idxptr != NULL;
			 prev_oid_ptr = idxptr, idxptr = idxptr->next_oid) {
	if ((res = snmp_oid_compare(varbind->name, varbind->name_length,
					idxptr->varbind.name,
					idxptr->varbind.name_length)) <= 0 )
		break;
    }

		/*  Found the OID - now look at the registered indices */
    if ( res == 0 && idxptr ) {
	if ( varbind->type != idxptr->varbind.type )
	    return NULL;		/* wrong type */

			/*
			 * If we've been asked for an arbitrary value,
			 *	then find the end of the list.
			 * Otherwise, locate the given value in the (sorted)
			 *	list of already allocated values
			 */
	if ( flags & ALLOCATE_ANY_INDEX ) {
            for(idxptr2 = idxptr ; idxptr2 != NULL;
		 prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
	    }
	}
	else {
            for(idxptr2 = idxptr ; idxptr2 != NULL;
		 prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
	        switch ( varbind->type ) {
		    case ASN_INTEGER:
			res2 = (*varbind->val.integer - *idxptr2->varbind.val.integer);
			break;
		    case ASN_OCTET_STR:
			i = SNMP_MIN(varbind->val_len, idxptr2->varbind.val_len);
			res2 = memcmp(varbind->val.string, idxptr2->varbind.val.string, i);
			break;
		    case ASN_OBJECT_ID:
			res2 = snmp_oid_compare(varbind->val.objid, varbind->val_len/sizeof(oid),
					idxptr2->varbind.val.objid,
					idxptr2->varbind.val_len/sizeof(oid));
			break;
		    default:
	    		return NULL;		/* wrong type */
	        }
	        if ( res2 <= 0 )
		    break;
	    }
	    if ( res2 == 0 )
		return NULL;			/* duplicate value */
	}
    }

		/*
		 * OK - we've now located where the new entry needs to
		 *	be fitted into the index registry tree		
		 * To recap:
		 *	'prev_oid_ptr' points to the head of the OID index
		 *	    list prior to this one.  If this is null, then
		 *	    it means that this is the first OID in the list.
		 *	'idxptr' points either to the head of this OID list,
		 *	    or the next OID (if this is a new OID request)
		 *	    These can be distinguished by the value of 'res'.
		 *
		 *	'prev_idx_ptr' points to the index entry that sorts
		 *	    immediately prior to the requested value (if any).
		 *	    If an arbitrary value is required, then this will
		 *	    point to the last allocated index.
		 *	    If this pointer is null, then either this is a new
		 *	    OID request, or the requested value is the first
		 *	    in the list.
		 *	'idxptr2' points to the next sorted index (if any)
		 *	    but is not actually needed any more.
		 *
		 *  Clear?  Good!
		 *	I hope you've been paying attention.
		 *	    There'll be a test later :-)
		 */

		/*
		 *	We proceed by creating the new entry
		 *	   (by copying the entry provided)
		 */
	new_index = (struct snmp_index *)malloc( sizeof( struct snmp_index ));
	if (new_index == NULL)
	    return NULL;
	if (snmp_clone_var( varbind, new_index ) != 0 ) {
	    free( new_index );
	    return NULL;
	}
	new_index->session = ss;

	if ( varbind->type == ASN_OCTET_STR && flags == ALLOCATE_THIS_INDEX )
	    new_index->varbind.val.string[new_index->varbind.val_len] = 0;

		/*
		 * If we've been given a value, then we can use that, but
		 *    otherwise, we need to create a new value for this entry.
		 * Note that ANY_INDEX and NEW_INDEX are both covered by this
		 *   test (since NEW_INDEX & ANY_INDEX = ANY_INDEX, remember?)
		 */
	if ( flags & ALLOCATE_ANY_INDEX ) {
	    if ( prev_idx_ptr ) {
		if ( snmp_clone_var( prev_idx_ptr, new_index ) != 0 ) {
		    free( new_index );
		    return NULL;
		}
	    }
	    else
		new_index->varbind.val.string = new_index->varbind.buf;

	    switch ( varbind->type ) {
		case ASN_INTEGER:
		    if ( prev_idx_ptr ) {
			(*new_index->varbind.val.integer)++; 
		    }
		    else
			*(new_index->varbind.val.integer) = 1;
		    new_index->varbind.val_len = sizeof(long);
		    break;
		case ASN_OCTET_STR:
		    if ( prev_idx_ptr ) {
			i =  new_index->varbind.val_len-1;
			while ( new_index->varbind.buf[ i ] == 'z' ) {
			    new_index->varbind.buf[ i ] = 'a';
			    i--;
			    if ( i < 0 ) {
				i =  new_index->varbind.val_len;
			        new_index->varbind.buf[ i ] = 'a';
			        new_index->varbind.buf[ i+1 ] = 0;
			    }
			}
			new_index->varbind.buf[ i ]++;
		    }
		    else
		        strcpy(new_index->varbind.buf, "aaaa");
		    new_index->varbind.val_len = strlen(new_index->varbind.buf);
		    break;
		case ASN_OBJECT_ID:
		    if ( prev_idx_ptr ) {
			i =  prev_idx_ptr->varbind.val_len/sizeof(oid) -1;
			while ( new_index->varbind.val.objid[ i ] == 255 ) {
			    new_index->varbind.val.objid[ i ] = 1;
			    i--;
			    if ( i == 0 && new_index->varbind.val.objid[0] == 2 ) {
			        new_index->varbind.val.objid[ 0 ] = 1;
				i =  new_index->varbind.val_len/sizeof(oid);
			        new_index->varbind.val.objid[ i ] = 0;
				new_index->varbind.val_len += sizeof(oid);
			    }
			}
			new_index->varbind.val.objid[ i ]++;
		    }
		    else {
			/* If the requested OID name is small enough,
			 *   append another OID (1) and use this as the
			 *   default starting value for new indexes.
			 */
		        if ( (varbind->name_length+1) * sizeof(oid) <= 40 ) {
			    for ( i = 0 ; i<varbind->name_length ; i++ )
			        new_index->varbind.val.objid[i] = varbind->name[i];
			    new_index->varbind.val.objid[varbind->name_length] = 1;
			    new_index->varbind.val_len =
					(varbind->name_length+1) * sizeof(oid);
		        }
		        else {
			    /* Otherwise use '.1.1.1.1...' */
			    i = 40/sizeof(oid);
			    if ( i > 4 )
				i = 4;
			    new_index->varbind.val_len = i * (sizeof(oid));
			    for (i-- ; i>=0 ; i-- )
			        new_index->varbind.val.objid[i] = 1;
		        }
		    }
		    break;
		default:
		    free( new_index );
		    return NULL;	/* Index type not supported */
	    }
	}

		/*
		 * Right - we've set up the new entry.
		 * All that remains is to link it into the tree.
		 * There are a number of possible cases here,
		 *   so watch carefully.
		 */
	if ( prev_idx_ptr ) {
	    new_index->next_idx = prev_idx_ptr->next_idx;
	    new_index->next_oid = prev_idx_ptr->next_oid;
	    prev_idx_ptr->next_idx = new_index;
	}
	else {
	    if ( res == 0 && idxptr ) {
	        new_index->next_idx = idxptr;
	        new_index->next_oid = idxptr->next_oid;
	    }
	    else {
	        new_index->next_idx = NULL;
	        new_index->next_oid = idxptr;
	    }

	    if ( prev_oid_ptr ) {
		while ( prev_oid_ptr ) {
		    prev_oid_ptr->next_oid = new_index;
		    prev_oid_ptr = prev_oid_ptr->next_idx;
		}
	    }
	    else
	        snmp_index_head = new_index;
	}
    return (struct variable_list*)new_index;
}

	/*
	 * Release an allocated index,
	 *   to allow it to be used elsewhere
	 */
int
release_index(struct variable_list *varbind)
{
    return( unregister_index( varbind, TRUE, NULL ));
}

	/*
	 * Completely remove an allocated index,
	 *   due to errors in the registration process.
	 */
int
remove_index(struct variable_list *varbind, struct snmp_session *ss)
{
    return( unregister_index( varbind, FALSE, ss ));
}

int
unregister_index(struct variable_list *varbind, int remember, struct snmp_session *ss)
{
    struct snmp_index *new_index, *idxptr, *idxptr2;
    struct snmp_index *prev_oid_ptr, *prev_idx_ptr;
    int res, res2, i;

#if defined(USING_AGENTX_SUBAGENT_MODULE) && !defined(TESTING)
    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE) == SUB_AGENT )
	return( agentx_unregister_index( ss, varbind ));
#endif
		/* Look for the requested OID entry */
    prev_oid_ptr = NULL;
    prev_idx_ptr = NULL;
    for( idxptr = snmp_index_head ; idxptr != NULL;
			 prev_oid_ptr = idxptr, idxptr = idxptr->next_oid) {
	if ((res = snmp_oid_compare(varbind->name, varbind->name_length,
					idxptr->varbind.name,
					idxptr->varbind.name_length)) <= 0 )
		break;
    }

    if ( res != 0 )
	return INDEX_ERR_NOT_ALLOCATED;
    if ( varbind->type != idxptr->varbind.type )
	return INDEX_ERR_WRONG_TYPE;

    for(idxptr2 = idxptr ; idxptr2 != NULL;
		prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
	i = SNMP_MIN(varbind->val_len, idxptr2->varbind.val_len);
	res2 = memcmp(varbind->val.string, idxptr2->varbind.val.string, i);
	if ( res2 <= 0 )
	    break;
    }
    if ( res2 != 0 )
	return INDEX_ERR_NOT_ALLOCATED;
    if ( ss != idxptr2->session )
	return INDEX_ERR_WRONG_SESSION;

		/*
		 *  If this is a "normal" index unregistration,
		 *	mark the index entry as unused, but leave
		 *	it in situ.  This allows differentiation
		 *	between ANY_INDEX and NEW_INDEX
		 */
    if ( remember ) {
	idxptr2->session = NULL;	/* Unused index */
	return SNMP_ERR_NOERROR;
    }
		/*
		 *  If this is a failed attempt to register a
		 *	number of indexes, the successful ones
		 *	must be removed completely.
		 */
    if ( prev_idx_ptr ) {
	prev_idx_ptr->next_idx = idxptr2->next_idx;
    }
    else if ( prev_oid_ptr ) {
	if ( idxptr2->next_idx )	/* Use p_idx_ptr as a temp variable */
	    prev_idx_ptr = idxptr2->next_idx;
	else
	    prev_idx_ptr = idxptr2->next_oid;
	while ( prev_oid_ptr ) {
	    prev_oid_ptr->next_oid = prev_idx_ptr;
	    prev_oid_ptr = prev_oid_ptr->next_idx;
	}
    }
    else {
	if ( idxptr2->next_idx )
	    snmp_index_head = idxptr2->next_idx;
	else
	    snmp_index_head = idxptr2->next_oid;
    }
    snmp_free_var( (struct variable_list *)idxptr2 );
    return SNMP_ERR_NOERROR;
}


void dump_registry( void )
{
    struct subtree *myptr, *myptr2;
    struct snmp_index *idxptr, *idxptr2;
    char start_oid[SPRINT_MAX_LEN];
    char end_oid[SPRINT_MAX_LEN];

    for( myptr = subtrees ; myptr != NULL; myptr = myptr->next) {
	sprint_objid(start_oid, myptr->start, myptr->start_len);
	sprint_objid(end_oid, myptr->end, myptr->end_len);
	printf("%c %s - %s %c\n",
		( myptr->variables ? ' ' : '(' ),
		  start_oid, end_oid,
		( myptr->variables ? ' ' : ')' ));
	for( myptr2 = myptr ; myptr2 != NULL; myptr2 = myptr2->children) {
	    if ( myptr2->label && myptr2->label[0] )
		printf("\t%s\n", myptr2->label);
	}
    }

    if ( snmp_index_head )
	printf("\nIndex Allocations:\n");
    for( idxptr = snmp_index_head ; idxptr != NULL; idxptr = idxptr->next_oid) {
	sprint_objid(start_oid, idxptr->varbind.name, idxptr->varbind.name_length);
	printf("%s indexes:\n");
        for( idxptr2 = idxptr ; idxptr2 != NULL; idxptr2 = idxptr2->next_idx) {
	    switch( idxptr2->varbind.type ) {
		case ASN_INTEGER:
		    printf("    %c %d %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  *idxptr2->varbind.val.integer,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		case ASN_OCTET_STR:
		    printf("    %c %s %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  idxptr2->varbind.val.string,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		case ASN_OBJECT_ID:
		    sprint_objid(end_oid, idxptr2->varbind.val.objid,
				idxptr2->varbind.val_len/sizeof(oid));
		    printf("    %c %s %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  end_oid,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		default:
		    printf("unsupported type (%d)\n",
				idxptr2->varbind.type);
	    }
	}
    }
}

#ifdef TESTING
struct variable_list varbind;

void
test_string_register( int n, char *cp )
{
    varbind.name[4] = n;
    if (register_string_index(varbind.name, varbind.name_length, cp) == NULL)
	printf("allocating %s failed\n", cp);
}

void
test_int_register( int n, int val )
{
    varbind.name[4] = n;
    if (register_int_index( varbind.name, varbind.name_length, val ) == NULL )
	printf("allocating %d/%d failed\n", n, val);
}

void
test_oid_register( int n, int subid )
{
    struct variable_list *res;

    varbind.name[4] = n;
    if ( subid != -1 ) {
        varbind.val.objid[5] = subid;
	res = register_oid_index(varbind.name, varbind.name_length,
		    varbind.val.objid,
		    varbind.val_len/sizeof(oid) );
    }
    else
	res = register_oid_index(varbind.name, varbind.name_length, NULL, 0);

    if (res == NULL )
	printf("allocating %d/%d failed\n", n, subid);
}

main()
{
    oid name[] = { 1, 2, 3, 4, 0 };
    int i;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    snmp_set_var_objid( &varbind, name, 5 );
    varbind.type = ASN_OCTET_STR;
		/*
		 * Test index structure linking:
		 *	a) sorted by OID
		 */
    test_string_register( 20, "empty OID" );
    test_string_register( 10, "first OID" );
    test_string_register( 40, "last OID" );
    test_string_register( 30, "middle OID" );

		/*
		 *	b) sorted by index value
		 */
    test_string_register( 25, "eee: empty IDX" );
    test_string_register( 25, "aaa: first IDX" );
    test_string_register( 25, "zzz: last IDX" );
    test_string_register( 25, "mmm: middle IDX" );
    printf("This next one should fail....\n");
    test_string_register( 25, "eee: empty IDX" );	/* duplicate */
    printf("done\n");

		/*
		 *	c) test initial index linking
		 */
    test_string_register( 5, "eee: empty initial IDX" );
    test_string_register( 5, "aaa: replace initial IDX" );

		/*
		 *	Did it all work?
		 */
    dump_registry();
    snmp_index_head = NULL;	/* memory leak, but we're only testing */   
		/*
		 *  Now test index allocation
		 *	a) integer values
		 */
    test_int_register( 10, -1 );	/* empty */
    test_int_register( 10, -1 );	/* append */
    test_int_register( 10, 10 );	/* append exact */
    printf("This next one should fail....\n");
    test_int_register( 10, 10 );	/* exact duplicate */
    printf("done\n");
    test_int_register( 10, -1 );	/* append */
    test_int_register( 10,  5 );	/* insert exact */

		/*
		 *	b) string values
		 */
    test_string_register( 20, NULL );		/* empty */
    test_string_register( 20, NULL );		/* append */
    test_string_register( 20, "aaaz" );
    test_string_register( 20, NULL );		/* minor rollover */
    test_string_register( 20, "zzzz" );
    test_string_register( 20, NULL );		/* major rollover */

		/*
		 *	c) OID values
		 */
    
    test_oid_register( 30, -1 );	/* empty */
    test_oid_register( 30, -1 );	/* append */

    varbind.val_len = varbind.name_length*sizeof(oid);
    memcpy( varbind.buf, varbind.name, varbind.val_len);
    varbind.val.objid = (oid*) varbind.buf;
    varbind.val_len += sizeof(oid);

    test_oid_register( 30, 255 );	/* append exact */
    test_oid_register( 30, -1 );	/* minor rollover */
    test_oid_register( 30, 100 );	/* insert exact */
    printf("This next one should fail....\n");
    test_oid_register( 30, 100 );	/* exact duplicate */
    printf("done\n");

    varbind.val.objid = (oid*)varbind.buf;
    for ( i=0; i<6; i++ )
	varbind.val.objid[i]=255;
    varbind.val.objid[0]=1;
    test_oid_register( 30, 255 );	/* set up rollover  */
    test_oid_register( 30, -1 );	/* medium rollover */

    for ( i=0; i<6; i++ )
	varbind.val.objid[i]=255;
    varbind.val.objid[0]=2;
    test_oid_register( 30, 255 );	/* set up rollover  */
    test_oid_register( 30, -1 );	/* major rollover */

		/*
		 *	Did it all work?
		 */
    dump_registry();

		/*
		 *	Test the various "invalid" requests
		 *	(unsupported types, mis-matched types, etc)
		 */
    printf("The rest of these should fail....\n");
    test_oid_register( 10, -1 );
    test_oid_register( 10, 100 );
    test_oid_register( 20, -1 );
    test_oid_register( 20, 100 );
    test_string_register( 10, NULL );
    test_string_register( 10, "aaaa" );
    test_string_register( 30, NULL );
    test_string_register( 30, "aaaa" );
    test_int_register( 20, -1 );
    test_int_register( 20,  1 );
    test_int_register( 30, -1 );
    test_int_register( 30,  1 );
    printf("done - this dump should be the same as before\n");
    dump_registry();
}
#endif
