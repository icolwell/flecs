#include <string.h>
#include <assert.h>
#include "include/private/flecs.h"
#include "include/util/time.h"

const ecs_array_params_t column_arr_params = {
    .element_size = sizeof(ecs_system_column_t)
};

static
ecs_entity_t components_contains(
    ecs_world_t *world,
    ecs_type_t table_type,
    ecs_type_t type,
    ecs_entity_t *entity_out,
    bool match_all)
{
    ecs_array_t *components = ecs_type_get(world, &world->main_stage, table_type);
    assert(components != NULL);

    uint32_t i, count = ecs_array_count(components);
    for (i = 0; i < count; i ++) {
        ecs_entity_t h = *(ecs_entity_t*)ecs_array_get(
            components, &handle_arr_params, i);

        if (ecs_has(world, h, EcsPrefab)) {
            return 0;
        }

        uint64_t row_64 = ecs_map_get64(world->main_stage.entity_index, h);
        assert(row_64 != 0);

        ecs_row_t row = ecs_to_row(row_64);
        ecs_entity_t component = ecs_type_contains(
            world, &world->main_stage, row.type_id, type, match_all, true);
        if (component != 0) {
            if (entity_out) *entity_out = h;
            return component;
        }
    }

    return 0;
}

static
bool components_contains_component(
    ecs_world_t *world,
    ecs_type_t table_type,
    ecs_entity_t component,
    ecs_entity_t *entity_out)
{
    ecs_array_t *components = ecs_type_get(world, &world->main_stage, table_type);
    assert(components != NULL);

    uint32_t i, count = ecs_array_count(components);
    for (i = 0; i < count; i ++) {
        ecs_entity_t h = *(ecs_entity_t*)ecs_array_get(
            components, &handle_arr_params, i);

        uint64_t row_64 = ecs_map_get64(world->main_stage.entity_index, h);
        assert(row_64 != 0);

        ecs_row_t row = ecs_to_row(row_64);
        bool result = ecs_type_contains_component(
            world, &world->main_stage, row.type_id, component, true);
        if (result) {
            if (entity_out) *entity_out = h;
            return true;
        }
    }

    return false;
}

/* Special indexes in table_data array */
#define TABLE_INDEX (0)
#define REFS_INDEX (1)
#define REFS_COUNT (2)
#define COMPONENTS_INDEX (3)
#define COLUMNS_INDEX (4)

/* Get ref array for system table */
static
ecs_system_ref_t* get_ref_data(
    EcsColSystem *system_data,
    int32_t *table_data)
{
    ecs_system_ref_t *ref_data = NULL;

    if (!system_data->refs) {
        system_data->refs = ecs_array_new(&system_data->ref_params, 1);
    }

    if (!table_data[REFS_INDEX]) {
        ref_data = ecs_array_add(
            &system_data->refs, &system_data->ref_params);
        table_data[REFS_INDEX] = ecs_array_count(system_data->refs);
    } else {
        ref_data = ecs_array_get(
            system_data->refs, &system_data->ref_params, table_data[REFS_INDEX] - 1);
    }

    return ref_data;
}

/* Get actual entity on which specified component is stored */
static
ecs_entity_t get_entity_for_component(
    ecs_world_t *world,
    ecs_entity_t entity,
    ecs_type_t type_id,
    ecs_entity_t component)
{
    if (entity) {
        ecs_row_t row = ecs_to_row(ecs_map_get64(world->main_stage.entity_index, entity));
        type_id = row.type_id;
    }

    ecs_array_t *type = ecs_type_get(world, NULL, type_id);
    ecs_entity_t *buffer = ecs_array_buffer(type);
    uint32_t i, count = ecs_array_count(type);

    for (i = 0; i < count; i ++) {
        if (buffer[i] == component) {
            break;
        }
    }

    if (i == count) {
        ecs_entity_t prefab = ecs_map_get64(world->prefab_index, type_id);
        if (prefab) {
            return get_entity_for_component(world, prefab, 0, component);
        }
    }

    /* This function must only be called if it has already been validated that
     * a component is available for a given type or entity */
    assert(entity != 0);

    return entity;
}

/** Add table to system, compute offsets for system components in table rows */
static
void add_table(
    ecs_world_t *world,
    ecs_entity_t system,
    EcsColSystem *system_data,
    ecs_table_t *table)
{
    int32_t *table_data;
    ecs_system_ref_t *ref_data = NULL;
    ecs_type_t table_type = table->type_id;
    uint32_t i = COLUMNS_INDEX;
    uint32_t ref = 0;
    uint32_t column_count = ecs_array_count(system_data->base.columns);

    /* Initially always add table to inactive group. If the system is registered
     * with the table and the table is not empty, the table will send an
     * activate signal to the system. */
    table_data = ecs_array_add(
        &system_data->inactive_tables, &system_data->table_params);

    /* Add element to array that contains components for this table. Tables
     * typically share the same component list, unless the system contains OR
     * expressions in the signature. In that case, the system can match against
     * tables that have different components for a column. */
    ecs_entity_t *component_data = ecs_array_add(
        &system_data->components, &system_data->component_params);

    /* Table index is at element 0 */
    table_data[TABLE_INDEX] = ecs_array_get_index(
        world->main_stage.tables, &table_arr_params, table);

    /* Index in ref array is at element 1 (0 means no refs) */
    table_data[REFS_INDEX] = 0;

    /* Index in components array is at element 2 */
    table_data[COMPONENTS_INDEX] = ecs_array_count(system_data->components) - 1;

    /* Walk columns parsed from the system signature */
    ecs_system_column_t *columns = ecs_array_buffer(system_data->base.columns);
    uint32_t c, count = ecs_array_count(system_data->base.columns);

    for (c = 0; c < count; c ++) {
        ecs_system_column_t *column = &columns[c];
        ecs_entity_t entity = 0, component = 0;

        /* Column that retrieves data from an entity */
        if (column->kind == EcsFromSelf || column->kind == EcsFromEntity) {
            if (column->oper_kind == EcsOperAnd) {
                component = column->is.component;
            } else if (column->oper_kind == EcsOperOptional) {
                component = column->is.component;
            } else if (column->oper_kind == EcsOperOr) {
                component = ecs_type_contains(
                    world, &world->main_stage, table_type, column->is.type, 
                    false, true);
            }

            if (column->kind == EcsFromEntity) {
                entity = column->source;
            }

        /* Column that just passes a handle to the system (no data) */
        } else if (column->kind == EcsFromId) {
            component = column->is.component;
            table_data[i] = 0;

        /* Column that retrieves data from a component */
        } else if (column->kind == EcsFromContainer) {
            if (column->oper_kind == EcsOperAnd ||
                column->oper_kind == EcsOperOptional)
            {
                component = column->is.component;
                components_contains_component(
                    world, table_type, component, &entity);

            } else if (column->oper_kind == EcsOperOr) {
                component = components_contains(
                    world,
                    table_type,
                    column->is.type,
                    &entity,
                    false);
            }

        /* Column that retrieves data from a system */
        } else if (column->kind == EcsFromSystem) {
            if (column->oper_kind == EcsOperAnd) {
                component = column->is.component;
            }

            entity = system;

        /* Column that retrieves singleton components */
        } else if (column->kind == EcsFromSingleton) {
            component = column->is.component;
            entity = 0;
        }

        /* This column does not retrieve data from a static entity (either
         * EcsFromSystem or EcsFromContainer) and is not just a handle */
        if (!entity && column->kind != EcsFromId) {
            if (component) {
                /* Retrieve offset for component */
                table_data[i] = ecs_type_index_of(table->type, component);

                /* If column is found, add one to the index, as column zero in
                 * a table is reserved for entity id's */
                if (table_data[i] != -1) {
                    table_data[i] ++;

                    /* Check if component is a tag. If it is, set table_data to
                     * zero, so that a system won't try to access the data */
                    EcsComponent *data = ecs_get_ptr(
                        world, component, EcsComponent);

                    if (!data || !data->size) {
                        table_data[i] = 0;
                    }
                }
                
                /* ecs_table_column_offset may return -1 if the component comes
                 * from a prefab. If so, the component will be resolved as a
                 * reference (see below) */
            } else {
                /* Columns with a NOT expression have no data */
                table_data[i] = 0;
            }
        }

        if (column->oper_kind == EcsOperOptional) {
            /* If table doesn't have the field, mark it as no data */
            if (!ecs_type_contains_component(
                world, &world->main_stage, table_type, component, true))
            {
                table_data[i] = 0;
            }
        }

        /* If entity is set, or component is not found in table, add it as a ref
         * to data of a specific entity. */
        if (entity || table_data[i] == -1 || column->kind == EcsFromSingleton) {
            if (ecs_has(world, component, EcsComponent)) {
                EcsComponent *component_data = ecs_get_ptr(
                        world, component, EcsComponent);
                
                if (component_data->size) {
                    if (!ref_data) {
                        ref_data = get_ref_data(system_data, table_data);
                        table_data[REFS_COUNT] = 0;
                    }

                    /* Find the entity for the component */
                    if (column->kind == EcsFromSingleton) {
                        ref_data[ref].entity = 0;
                    } else if (column->kind == EcsFromEntity) {
                        ref_data[ref].entity = entity;
                    } else {
                        ref_data[ref].entity = get_entity_for_component(
                            world, entity, table_type, component);
                    }

                    ref_data[ref].component = component;
                    ref ++;

                    /* Negative number indicates ref instead of offset to ecs_data */
                    table_data[i] = -ref;
                    table_data[REFS_COUNT] ++;
                }
            }
        }

        /* component_data index is not offset by anything */
        component_data[i - COLUMNS_INDEX] = component;

        i ++;
    }

    if (ref_data && ref < column_count) {
        ref_data[ref].entity = 0;
    }

    ecs_table_register_system(world, table, system);
}

/* Match table with system */
static
bool match_table(
    ecs_world_t *world,
    ecs_table_t *table,
    EcsColSystem *system_data)
{
    ecs_type_t type, table_type;
    table_type = table->type_id;

    if (ecs_type_contains_component(
        world, &world->main_stage, table_type, EEcsPrefab, false))
    {
        /* Never match prefabs */
        return false;
    }

    type = system_data->base.and_from_entity;

    if (type && !ecs_type_contains(
        world, &world->main_stage, table_type, type, true, true))
    {
        return false;
    }

    uint32_t i, column_count = ecs_array_count(system_data->base.columns);
    ecs_system_column_t *buffer = ecs_array_buffer(system_data->base.columns);

    for (i = 0; i < column_count; i ++) {
        ecs_system_column_t *elem = &buffer[i];
        EcsSystemExprElemKind elem_kind = elem->kind;
        EcsSystemExprOperKind oper_kind = elem->oper_kind;

        if (oper_kind == EcsOperAnd) {
            if (elem_kind == EcsFromSelf) {
                /* Already validated */
            } else if (elem_kind == EcsFromContainer) {
                if (!components_contains_component(
                    world, table_type, elem->is.component, NULL))
                {
                    return false;
                }
            }
        } else if (oper_kind == EcsOperOr) {
            type = elem->is.type;
            if (elem_kind == EcsFromSelf) {
                if (!ecs_type_contains(
                    world, &world->main_stage, table_type, type, false, true))
                {
                    return false;
                }
            } else if (elem_kind == EcsFromContainer) {
                if (!components_contains(
                    world, table_type, type, NULL, false))
                {
                    return false;
                }
            }
        }
    }

    type = system_data->base.not_from_entity;
    if (type && ecs_type_contains(
        world, &world->main_stage, table_type, type, false, true))
    {
        return false;
    }

    type = system_data->base.not_from_component;
    if (type && components_contains(
        world, table_type, type, NULL, false))
    {
        return false;
    }

    return true;
}

/** Match existing tables against system (table is created before system) */
static
void match_tables(
    ecs_world_t *world,
    ecs_entity_t system,
    EcsColSystem *system_data)
{
    ecs_table_t *buffer = ecs_array_buffer(world->main_stage.tables);
    uint32_t i, count = ecs_array_count(world->main_stage.tables);

    for (i = 0; i < count; i ++) {
        ecs_table_t *table = &buffer[i];
        if (match_table(world, table, system_data)) {
            add_table(world, system, system_data, table);
        }
    }
}

/* -- Private functions -- */

/** Match new table against system (table is created after system) */
void ecs_col_system_notify_of_table(
    ecs_world_t *world,
    ecs_entity_t system,
    ecs_table_t *table)
{
    EcsColSystem *system_data = ecs_get_ptr(world, system, EcsColSystem);
    assert(system_data != NULL);

    if (match_table(world, table, system_data)) {
        add_table(world, system, system_data, table);
    }
}

/** Table activation happens when a table was or becomes empty. Deactivated
 * tables are not considered by the system in the main loop. */
void ecs_system_activate_table(
    ecs_world_t *world,
    ecs_entity_t system,
    ecs_table_t *table,
    bool active)
{
    ecs_array_t *src_array, *dst_array;
    EcsColSystem *system_data = ecs_get_ptr(world, system, EcsColSystem);
    EcsSystemKind kind = system_data->base.kind;

    uint32_t table_index = ecs_array_get_index(
        world->main_stage.tables, &table_arr_params, table);

    if (active) {
        src_array = system_data->inactive_tables;
        dst_array = system_data->tables;
    } else {
        src_array = system_data->tables;
        dst_array = system_data->inactive_tables;
    }

    uint32_t i, count = ecs_array_count(src_array);
    for (i = 0; i < count; i ++) {
        uint32_t *index = ecs_array_get(
            src_array, &system_data->table_params, i);
        if (*index == table_index) {
            break;
        }
    }

    ecs_assert(i != count, ECS_INTERNAL_ERROR, "cannot find table to (de)activate");

    uint32_t src_count = ecs_array_move_index(
        &dst_array, src_array, &system_data->table_params, i);

    if (active) {
        uint32_t dst_count = ecs_array_count(dst_array);
        if (kind != EcsManual) {
            if (dst_count == 1 && system_data->base.enabled) {
                ecs_world_activate_system(
                    world, system, kind, true);
            }
        }
        system_data->tables = dst_array;
    } else {
        if (kind != EcsManual) {
            if (src_count == 0) {
                ecs_world_activate_system(
                    world, system, kind, false);
            }
        }
        system_data->inactive_tables = dst_array;
    }
}

/* -- Private API -- */

ecs_entity_t ecs_new_col_system(
    ecs_world_t *world,
    const char *id,
    EcsSystemKind kind,
    const char *sig,
    ecs_system_action_t action)
{
    uint32_t count = ecs_columns_count(sig);
    if (!count) {
        assert(0);
    }

    ecs_entity_t result = _ecs_new(
        world, world->t_col_system);

    EcsId *id_data = ecs_get_ptr(world, result, EcsId);
    *id_data = id;

    EcsColSystem *system_data = ecs_get_ptr(world, result, EcsColSystem);
    memset(system_data, 0, sizeof(EcsColSystem));
    system_data->base.action = action;
    system_data->base.enabled = true;
    system_data->base.signature = sig;
    system_data->base.time_spent = 0;
    system_data->base.columns = ecs_array_new(&column_arr_params, count);
    system_data->base.kind = kind;

    system_data->table_params.element_size = sizeof(int32_t) * (count + COLUMNS_INDEX);
    system_data->ref_params.element_size = sizeof(ecs_system_ref_t) * count;
    system_data->component_params.element_size = sizeof(ecs_entity_t) * count;
    system_data->period = 0;
    system_data->entity = result;

    system_data->components = ecs_array_new(
        &system_data->component_params, ECS_SYSTEM_INITIAL_TABLE_COUNT);
    system_data->tables = ecs_array_new(
        &system_data->table_params, ECS_SYSTEM_INITIAL_TABLE_COUNT);
    system_data->inactive_tables = ecs_array_new(
        &system_data->table_params, ECS_SYSTEM_INITIAL_TABLE_COUNT);

    if (ecs_parse_component_expr(
        world, sig, ecs_parse_component_action, system_data) != 0)
    {
        assert(0);
    }

    ecs_system_compute_and_families(world, &system_data->base);

    match_tables(world, result, system_data);

    ecs_entity_t *elem = NULL;

    if (kind == EcsManual) {
        elem = ecs_array_add(&world->on_demand_systems, &handle_arr_params);
    } else if (!ecs_array_count(system_data->tables)) {
        elem = ecs_array_add(&world->inactive_systems, &handle_arr_params);
    } else {
        if (kind == EcsOnUpdate) {
            elem = ecs_array_add(&world->on_update_systems, &handle_arr_params);
        } else if (kind == EcsOnValidate) {
            elem = ecs_array_add(&world->on_validate_systems, &handle_arr_params);            
        } else if (kind == EcsPreUpdate) {
            elem = ecs_array_add(&world->pre_update_systems, &handle_arr_params);
        } else if (kind == EcsPostUpdate) {
            elem = ecs_array_add(&world->post_update_systems, &handle_arr_params);
        } else if (kind == EcsOnLoad) {
            elem = ecs_array_add(&world->on_load_systems, &handle_arr_params);
        } else if (kind == EcsOnStore) {
            elem = ecs_array_add(&world->on_store_systems, &handle_arr_params);
        } else {
            ecs_abort(ECS_INVALID_PARAMETERS, NULL);
        }
    }

    *elem = result;

    return result;
}

/* -- Public API -- */

static
bool should_run(
    EcsColSystem *system_data,
    float period,
    float delta_time)
{
    float time_passed = system_data->time_passed + delta_time;

    delta_time = time_passed;

    if (time_passed >= period) {
        time_passed -= period;
        if (time_passed > period) {
            time_passed = 0;
        }

        system_data->time_passed = time_passed;
    } else {
        system_data->time_passed = time_passed;
        return false;
    }

    return true;
}

ecs_entity_t _ecs_run_w_filter(
    ecs_world_t *world,
    ecs_entity_t system,
    float delta_time,
    uint32_t offset,
    uint32_t limit,
    ecs_type_t filter,
    void *param)
{
    ecs_world_t *real_world = world;

    if (world->magic == ECS_THREAD_MAGIC) {
        real_world = ((ecs_thread_t*)world)->world; /* dispel the magic */
    }

    ecs_entity_info_t entity_info = {0};
    EcsColSystem *system_data = get_ptr(real_world, &real_world->main_stage, system, EEcsColSystem, false, false, &entity_info);
    assert(system_data != NULL);

    if (!system_data->base.enabled) {
        return 0;
    }

    ecs_stage_t *stage = ecs_get_stage(&real_world);

    float system_delta_time = delta_time + system_data->time_passed;
    float period = system_data->period;
    bool measure_time = real_world->measure_system_time;

    ecs_array_t *tables = system_data->tables;
    uint32_t tables_size = system_data->table_params.element_size;
    int32_t *table_first = ecs_array_buffer(tables);
    int32_t *table_last = ECS_OFFSET(table_first, tables_size * ecs_array_count(tables));

    if (table_first == table_last) {
        return 0;
    }

    if (period) {
        if (!should_run(system_data, period, delta_time)) {
            return 0;
        }
    }

    ecs_time_t time_start;
    if (measure_time) {
        ecs_os_get_time(&time_start);
    }

    uint32_t column_count = ecs_array_count(system_data->base.columns);
    uint32_t components_size = system_data->component_params.element_size;
    char *components = ecs_array_buffer(system_data->components);
    ecs_entity_t interrupted_by = 0;
    ecs_system_action_t action = system_data->base.action;
    bool offset_limit = (offset | limit) != 0;
    bool limit_set = limit != 0;
    void **ref_ptrs = ecs_os_alloca(void*, column_count);

    ecs_rows_t info = {
        .world = world,
        .system = system,
        .param = param,
        .column_count = column_count,
        .delta_time = system_delta_time,
        .frame_offset = offset,
        .ref_ptrs = ref_ptrs
    };

    int32_t *table = table_first;
    for (; table < table_last; table = ECS_OFFSET(table, tables_size)) {
        int32_t table_index = table[TABLE_INDEX];

        /* A system may introduce a new table if in the main thread. Make sure
         * world_tables points to the valid memory */
        ecs_table_t *world_tables = ecs_array_buffer(real_world->main_stage.tables);
        ecs_table_t *w_table = &world_tables[table_index];
        ecs_table_column_t *table_columns = w_table->columns;
        uint32_t first = 0, count = ecs_table_count(w_table);

        if (filter) {
            if (!ecs_type_contains(
                real_world, stage, w_table->type_id, filter, true, true))
            {
                continue;
            }
        }

        if (offset_limit) {
            if (offset) {
                if (offset > count) {
                    offset -= count;
                    continue;
                } else {
                    first += offset;
                    count -= offset;
                    offset = 0;
                }
            }

            if (limit) {
                if (limit > count) {
                    limit -= count;
                } else {
                    count = limit;
                    limit = 0;
                }
            } else if (limit_set) {
                break;
            }
        }

        if (!count) {
            continue;
        }

        uint32_t ref_index = table[REFS_INDEX];

        if (ref_index) {
            info.references = ecs_array_get(
                system_data->refs, &system_data->ref_params, ref_index - 1);

            /* Resolve references */
            int i, count = table[REFS_COUNT];

            for (i = 0; i < count; i ++) {
                ecs_entity_info_t entity_info = {0};

                info.ref_ptrs[i] = get_ptr(real_world, &real_world->main_stage,
                    info.references[i].entity, info.references[i].component, false, true, &entity_info);

                ecs_assert(info.ref_ptrs[i] != NULL, ECS_UNRESOLVED_REFERENCE, ecs_id(world, system));
            }
        } else {
            info.references = NULL;
        }

        info.columns =  &table[COLUMNS_INDEX];
        info.table_columns = table_columns;
        info.components = ECS_OFFSET(components,
            components_size * table[COMPONENTS_INDEX]);
        info.offset = first;
        info.count = count;

        ecs_entity_t *entity_buffer = 
                ecs_array_buffer(((ecs_table_column_t*)info.table_columns)[0].data);
        info.entities = &entity_buffer[first];
        
        action(&info);

        info.frame_offset += count;

        if (info.interrupted_by) {
            interrupted_by = info.interrupted_by;
            break;
        }
    }

    if (measure_time) {
        system_data->base.time_spent += ecs_time_measure(&time_start);
    }

    return interrupted_by;
}

ecs_entity_t ecs_run(
    ecs_world_t *world,
    ecs_entity_t system,
    float delta_time,
    void *param)
{
    return ecs_run_w_filter(world, system, delta_time, 0, 0, 0, param);
}
