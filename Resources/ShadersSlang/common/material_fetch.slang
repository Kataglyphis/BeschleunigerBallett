import scene_types;

// Owns the objectDescription binding; imports scene_types, not rt_types, whose capabilities conflict with `discard`.

[vk::binding(OBJECT_DESCRIPTION_BINDING, 0)] StructuredBuffer<ObjectDescription> objectDescription;

// The object description for a per-draw object index.
ObjectDescription fetch_object_description(uint objectIndex)
{
    return objectDescription[objectIndex];
}

// The caller passes primitiveID: PrimitiveIndex() is RT-only in Slang and would pull in RT capabilities here.
ObjMaterial fetch_material(ObjectDescription obj, uint primitiveID)
{
    MaterialIDs* materialIDs = (MaterialIDs*)obj.material_index_address;
    Materials* materials = (Materials*)obj.material_address;
    return materials->m[materialIDs->i[primitiveID]];
}