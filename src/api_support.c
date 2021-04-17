#include "private_api.h"

/** Parse callback that adds type to type identifier for ecs_new_type */
static
int parse_type_action(
    ecs_world_t *world,
    const char *name,
    const char *sig,
    int64_t column,
    ecs_term_t *term,
    void *data)
{
    ecs_vector_t **array = data;

    if (term->name) {
        ecs_parser_error(name, sig, column, 
            "column names not supported in type expression");
        goto error;
    }

    if (term->oper != EcsAnd) {
        ecs_parser_error(name, sig, column, 
            "operator other than AND not supported in type expression");
        goto error;
    }

    if (ecs_term_resolve(world, name, sig, column, term)) {
        goto error;
    }

    ecs_term_set_legacy(term);

    if (term->args[0].entity == 0) {
        /* Empty term */
        goto done;
    }

    if (term->from_kind != EcsFromOwned) {
        ecs_parser_error(name, sig, column, 
            "source modifiers not supported for type expressions");
        goto error;
    }

    if (term->args[0].entity != EcsThis) {
        ecs_parser_error(name, sig, column, 
            "subject other than this not supported in type expression");
        goto error;
    }

    ecs_entity_t* elem = ecs_vector_add(array, ecs_entity_t);
    *elem = term->id | term->role;

done:
    ecs_term_free(term);
    return 0;
error:
    ecs_term_free(term);
    return -1;
}

static
ecs_table_t* table_from_vec(
    ecs_world_t *world,
    ecs_vector_t *vec)
{
    ecs_entity_t *array = ecs_vector_first(vec, ecs_entity_t);
    int32_t count = ecs_vector_count(vec);

    ecs_entities_t entities = {
        .array = array,
        .count = count
    };

    return ecs_table_find_or_create(world, &entities);
}

static
EcsType type_from_vec(
    ecs_world_t *world,
    ecs_vector_t *vec)
{
    EcsType result = {0, 0};
    ecs_table_t *table = table_from_vec(world, vec);
    if (!table) {
        return result;
    }

    result.type = table->type;

    /* Create normalized type. A normalized type resolves all elements with an
     * AND flag and appends them to the resulting type, where the default type
     * maintains the original type hierarchy. */
    ecs_vector_t *normalized = NULL;

    ecs_entity_t *array = ecs_vector_first(vec, ecs_entity_t);
    int32_t i, count = ecs_vector_count(vec);
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = array[i];
        if (ECS_HAS_ROLE(e, AND)) {
            ecs_entity_t entity = ECS_PAIR_OBJECT(e);
            const EcsType *type_ptr = ecs_get(world, entity, EcsType);
            ecs_assert(type_ptr != NULL, ECS_INVALID_PARAMETER, 
                "flag must be applied to type");

            ecs_vector_each(type_ptr->normalized, ecs_entity_t, c_ptr, {
                ecs_entity_t *el = ecs_vector_add(&normalized, ecs_entity_t);
                *el = *c_ptr;
            })
        }       
    }

    /* Only get normalized type if it's different from the type */
    if (normalized) {
        ecs_entities_t normalized_array = ecs_type_to_entities(normalized);
        ecs_table_t *norm_table = ecs_table_traverse_add(
            world, table, &normalized_array, NULL);

        result.normalized = norm_table->type;

        ecs_vector_free(normalized);
    } else {
        result.normalized = result.type;
    }

    return result;
}

static
EcsType type_from_expr(
    ecs_world_t *world,
    const char *name,
    const char *expr)
{
    if (expr) {
        ecs_vector_t *vec = ecs_vector_new(ecs_entity_t, 1);
        ecs_parse_expr(world, name, expr, parse_type_action, &vec);
        EcsType result = type_from_vec(world, vec);
        ecs_vector_free(vec);
        return result;
    } else {
        return (EcsType){0, 0};
    }
}

/* If a name prefix is set with ecs_set_name_prefix, check if the entity name
 * has the prefix, and if so remove it. This enables using prefixed names in C
 * for components / systems while storing a canonical / language independent 
 * identifier. */
const char* ecs_name_from_symbol(
    ecs_world_t *world,
    const char *type_name)
{
    const char *prefix = world->name_prefix;
    if (type_name && prefix) {
        ecs_size_t len = ecs_os_strlen(prefix);
        if (!ecs_os_strncmp(type_name, prefix, len) && 
           (isupper(type_name[len]) || type_name[len] == '_')) 
        {
            if (type_name[len] == '_') {
                return type_name + len + 1;
            } else {
                return type_name + len;
            }
        }
    }

    return type_name;
}

void ecs_set_symbol(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name)
{
    if (!name) {
        return;
    }
    
    const char *e_name = ecs_name_from_symbol(world, name);

    EcsName *name_ptr = ecs_get_mut(world, e, EcsName, NULL);
    name_ptr->value = e_name;

    if (name_ptr->symbol) {
        ecs_os_free(name_ptr->symbol);
    }

    name_ptr->symbol = ecs_os_strdup(name);
}

ecs_entity_t ecs_lookup_w_id(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name)
{
    if (e) {
        /* If explicit id was provided but it does not exist in the world, make
         * sure it has the proper scope. This can happen when an entity was 
         * defined in another world. */
        if (!ecs_exists(world, e)) {
            ecs_entity_t scope = world->stage.scope;
            if (scope) {
                ecs_add_pair(world, e, EcsChildOf, scope);
            }
        }

        if (name) {
            /* Make sure name is the same */
            const char *existing = ecs_get_name(world, e);
            if (existing && strcmp(existing, name)) {
                ecs_abort(ECS_INCONSISTENT_NAME, name);
            }
            if (!existing) {
                ecs_set_symbol(world, e, name);
            }
        }
    }
    
    ecs_entity_t result = e;
    if (!result) {
        if (!name) {
            /* If neither an id nor name is specified, return 0 */
            return 0;
        }

        result = ecs_lookup(world, name);
    }
    
    return result;
}

/* -- Public functions -- */

ecs_type_t ecs_type_from_str(
    ecs_world_t *world,
    const char *expr)
{
    EcsType type = type_from_expr(world, NULL, expr);
    return type.normalized;
}

ecs_table_t* ecs_table_from_str(
    ecs_world_t *world,
    const char *expr)
{
    if (expr) {
        ecs_vector_t *vec = ecs_vector_new(ecs_entity_t, 1);
        ecs_parse_expr(world, NULL, expr, parse_type_action, &vec);
        ecs_table_t *result = table_from_vec(world, vec);
        ecs_vector_free(vec);
        return result;
    } else {
        return NULL;
    }
}

ecs_entity_t ecs_new_entity(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name,
    const char *expr)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Function cannot be called from a stage, use regular ecs_new */
    ecs_assert(world->magic == ECS_WORLD_MAGIC, ECS_INVALID_PARAMETER, NULL);

    ecs_entity_t result = ecs_lookup_w_id(world, e, name);
    if (!result) {
        result = ecs_new(world, 0);
        ecs_set_symbol(world, result, name);
    }
    
    EcsType type = type_from_expr(world, name, expr);

    ecs_add_type(world, result, type.normalized);

    return result;
}

ecs_entity_t ecs_new_prefab(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name,
    const char *expr)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Function cannot be called from a stage, use regular ecs_new */
    ecs_assert(world->magic == ECS_WORLD_MAGIC, ECS_INVALID_PARAMETER, NULL);

    ecs_entity_t result = ecs_lookup_w_id(world, e, name);
    if (!result) {
        result = ecs_new(world, 0);
        ecs_set_symbol(world, result, name);
    }

    ecs_add_id(world, result, EcsPrefab);

    EcsType type = type_from_expr(world, name, expr);
    ecs_add_type(world, result, type.normalized);

    return result;
}

ecs_entity_t ecs_new_component(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name,
    size_t size,
    size_t alignment)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_stage_from_world(&world);

    bool is_readonly = world->is_readonly;
    bool found = false;

    /* If world is in progress component may be registered, but only when not
     * in multithreading mode. */
    if (is_readonly) {
        ecs_assert(ecs_get_stage_count(world) <= 1, 
            ECS_INVALID_WHILE_ITERATING, NULL);

        /* Component creation should not be deferred */
        world->is_readonly = false;
    }

    ecs_entity_t result = ecs_lookup_w_id(world, e, name);
    if (!result) {
        result = ecs_new_component_id(world);
        found = true;
    }

    if (found) {
        ecs_set_symbol(world, result, name);
    }

    bool added = false;
    EcsComponent *ptr = ecs_get_mut(world, result, EcsComponent, &added);

    if (added) {
        ptr->size = ecs_from_size_t(size);
        ptr->alignment = ecs_from_size_t(alignment);
    } else {
        if (ptr->size != ecs_from_size_t(size)) {
            ecs_abort(ECS_INVALID_COMPONENT_SIZE, name);
        }
        if (ptr->alignment != ecs_from_size_t(alignment)) {
            ecs_abort(ECS_INVALID_COMPONENT_SIZE, name);
        }
    }

    ecs_modified(world, result, EcsComponent);

    if (e > world->stats.last_component_id && e < ECS_HI_COMPONENT_ID) {
        world->stats.last_component_id = e + 1;
    }

    if (is_readonly) {
        world->is_readonly = true;
    }

    ecs_assert(result != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(ecs_has(world, result, EcsComponent), ECS_INTERNAL_ERROR, NULL);

    return result;
}

ecs_entity_t ecs_new_type(
    ecs_world_t *world,
    ecs_entity_t e,
    const char *name,
    const char *expr)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_stage_from_world(&world);

    ecs_entity_t result = ecs_lookup_w_id(world, e, name);
    if (!result) {
        result = ecs_new_entity(world, 0, name, NULL);
    }
    
    EcsType type_parsed = type_from_expr(world, name, expr);

    bool added = false;
    EcsType *type = ecs_get_mut(world, result, EcsType, &added);
    if (added) {
        type->type = type_parsed.type;
        type->normalized = type_parsed.normalized;
    } else {
        if (type->type != type_parsed.type) {
            ecs_abort(ECS_ALREADY_DEFINED, name);
        }

        if (type->normalized != type_parsed.normalized) {
            ecs_abort(ECS_ALREADY_DEFINED, name);
        }
    }     

    /* This will allow the type to show up in debug tools */
    ecs_map_set(world->type_handles, (uintptr_t)type_parsed.type, &result);

    return result;
}
