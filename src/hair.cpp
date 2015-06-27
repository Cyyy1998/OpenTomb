#include <algorithm>
#include <cmath>
#include <bullet/LinearMath/btScalar.h>
#include "hair.h"
#include "mesh.h"
#include "render.h"

bool Hair::create(HairSetup *setup, std::shared_ptr<Entity> parent_entity)
{
    // No setup or parent to link to - bypass function.

    if( (!parent_entity) || (!setup)                           ||
        (setup->m_linkBody >= parent_entity->m_bf.bone_tag_count) ||
        (!(parent_entity->m_bt.bt_body[setup->m_linkBody]))         ) return false;

    skeletal_model_p model = World_GetModelByID(&engine_world, setup->m_model);

    // No model to link to - bypass function.

    if((!model) || (model->mesh_count == 0)) return false;

    // Setup engine container. FIXME: DOESN'T WORK PROPERLY ATM.

    m_container.reset( new EngineContainer() );
    m_container->room = parent_entity->m_self->room;
    m_container->object_type = OBJECT_HAIR;
    m_container->object = shared_from_this();

    // Setup initial hair parameters.

    m_ownerChar = parent_entity;       // Entity to refer to.
    m_ownerBody = setup->m_linkBody;    // Entity body to refer to.

    // Setup initial position / angles.

    btTransform owner_body_transform = parent_entity->m_transform * parent_entity->m_bf.bone_tags[m_ownerBody].full_transform;
    // Number of elements (bodies) is equal to number of hair meshes.

    m_elements.clear();
    m_elements.resize(model->mesh_count);

    // Root index should be always zero, as it is how engine determines that it is
    // connected to head and renders it properly. Tail index should be always the
    // last element of the hair, as it indicates absence of "child" constraint.

    m_rootIndex = 0;
    m_tailIndex = m_elements.size()-1;

    // Weight step is needed to determine the weight of each hair body.
    // It is derived from root body weight and tail body weight.

    btScalar weight_step = ((setup->m_rootWeight - setup->m_tailWeight) / m_elements.size());
    btScalar current_weight = setup->m_rootWeight;

    for(size_t i=0; i<m_elements.size(); i++)
    {
        // Point to corresponding mesh.

        m_elements[i].mesh = model->mesh_tree[i].mesh_base;

        // Begin creating ACTUAL physical hair mesh.

        btVector3   localInertia(0, 0, 0);
        btTransform startTransform;

        // Make collision shape out of mesh.

        m_elements[i].shape.reset( BT_CSfromMesh(m_elements[i].mesh, true, true, false) );
        m_elements[i].shape->calculateLocalInertia((current_weight * setup->m_hairInertia), localInertia);

        // Decrease next body weight to weight_step parameter.

        current_weight -= weight_step;

        // Initialize motion state for body.

        startTransform = owner_body_transform;
        btDefaultMotionState* motionState = new btDefaultMotionState(startTransform);

        // Make rigid body.

        m_elements[i].body.reset( new btRigidBody(current_weight, motionState, m_elements[i].shape.get(), localInertia) );

        // Damping makes body stop in space by itself, to prevent it from continous movement.

        m_elements[i].body->setDamping(setup->m_hairDamping[0], setup->m_hairDamping[1]);

        // Restitution and friction parameters define "bounciness" and "dullness" of hair.

        m_elements[i].body->setRestitution(setup->m_hairRestitution);
        m_elements[i].body->setFriction(setup->m_hairFriction);

        // Since hair is always moving with Lara, even if she's in still state (like, hanging
        // on a ledge), hair bodies shouldn't deactivate over time.

        m_elements[i].body->forceActivationState(DISABLE_DEACTIVATION);

        // Hair bodies must not collide with each other, and also collide ONLY with kinematic
        // bodies (e. g. animated meshes), or else Lara's ghost object or anything else will be able to
        // collide with hair!

        m_elements[i].body->setUserPointer(m_container.get());
        bt_engine_dynamicsWorld->addRigidBody(m_elements[i].body.get(), COLLISION_GROUP_CHARACTERS, COLLISION_GROUP_KINEMATIC);

        m_elements[i].body->activate();
    }

    // GENERATE CONSTRAINTS.
    // All constraints are generic 6-DOF type, as they seem perfect fit for hair.

    // Joint count is calculated from overall body amount multiplied by per-body constraint
    // count.

    m_joints.resize(m_elements.size());

    // If multiple joints per body is specified, joints are placed in circular manner,
    // with obvious step of (SIMD_2_PI) / joint count. It means that all joints will form
    // circle-like figure.

    int curr_joint = 0;

    for(size_t i=0; i<m_elements.size(); i++)
    {
        btScalar     body_length;

        // Each body width and height are used to calculate position of each joint.

        //btScalar body_width = fabs(elements[i].mesh->bb_max[0] - elements[i].mesh->bb_min[0]);
        //btScalar body_depth = fabs(elements[i].mesh->bb_max[3] - elements[i].mesh->bb_min[3]);

        btTransform localA; localA.setIdentity();
        btTransform localB; localB.setIdentity();

        btScalar joint_x = 0.0;
        btScalar joint_y = 0.0;

        std::shared_ptr<btRigidBody> prev_body;
        if(i == 0)  // First joint group
        {
            // Adjust pivot point A to parent body.

            localA.setOrigin(setup->m_headOffset + btVector3(joint_x, 0.0, joint_y));
            localA.getBasis().setEulerZYX(setup->m_rootAngle[0], setup->m_rootAngle[1], setup->m_rootAngle[2]);
            // Stealing this calculation because I need it for drawing
            m_ownerBodyHairRoot = localA;

            localB.setOrigin(btVector3(joint_x, 0.0, joint_y));
            localB.getBasis().setEulerZYX(0,-SIMD_HALF_PI,0);

            prev_body = parent_entity->m_bt.bt_body[m_ownerBody];   // Previous body is parent body.
        }
        else
        {
            // Adjust pivot point A to previous mesh's length, considering mesh overlap multiplier.

            body_length = fabs(m_elements[i-1].mesh->bb_max[1] - m_elements[i-1].mesh->bb_min[1]) * setup->m_jointOverlap;

            localA.setOrigin(btVector3(joint_x, body_length, joint_y));
            localA.getBasis().setEulerZYX(0,SIMD_HALF_PI,0);

            // Pivot point B is automatically adjusted by Bullet.

            localB.setOrigin(btVector3(joint_x, 0.0, joint_y));
            localB.getBasis().setEulerZYX(0,SIMD_HALF_PI,0);

            prev_body = m_elements[i-1].body;   // Previous body is preceiding hair mesh.
        }

        // Create 6DOF constraint.

        m_joints[curr_joint].reset( new btGeneric6DofConstraint(*prev_body, *(m_elements[i].body), localA, localB, true) );

        // CFM and ERP parameters are critical for making joint "hard" and link
        // to Lara's head. With wrong values, constraints may become "elastic".

        for(int axis=0;axis<=5;axis++)
        {
            m_joints[i]->setParam(BT_CONSTRAINT_STOP_CFM, setup->m_jointCfm, axis);
            m_joints[i]->setParam(BT_CONSTRAINT_STOP_ERP, setup->m_jointErp, axis);
        }

        if(i == 0)
        {
            // First joint group should be more limited in motion, as it is connected
            // right to the head. NB: Should we make it scriptable as well?

            m_joints[curr_joint]->setLinearLowerLimit(btVector3(0., 0., 0.));
            m_joints[curr_joint]->setLinearUpperLimit(btVector3(0., 0., 0.));
            m_joints[curr_joint]->setAngularLowerLimit(btVector3(-SIMD_HALF_PI,     0., -SIMD_HALF_PI*0.4));
            m_joints[curr_joint]->setAngularUpperLimit(btVector3(-SIMD_HALF_PI*0.3, 0.,  SIMD_HALF_PI*0.4));

            // Increased solver iterations make constraint even more stable.

            m_joints[curr_joint]->setOverrideNumSolverIterations(100);
        }
        else
        {
            // Normal joint with more movement freedom.

            m_joints[curr_joint]->setLinearLowerLimit(btVector3(0., 0., 0.));
            m_joints[curr_joint]->setLinearUpperLimit(btVector3(0., 0., 0.));
            m_joints[curr_joint]->setAngularLowerLimit(btVector3(-SIMD_HALF_PI*0.5, 0., -SIMD_HALF_PI*0.5));
            m_joints[curr_joint]->setAngularUpperLimit(btVector3( SIMD_HALF_PI*0.5, 0.,  SIMD_HALF_PI*0.5));

        }

        m_joints[curr_joint]->setDbgDrawSize(btScalar(5.f));    // Draw constraint axes.

        // Add constraint to the world.

        bt_engine_dynamicsWorld->addConstraint(m_joints[curr_joint].get(), true);

        curr_joint++;   // Point to the next joint.
    }

    createHairMesh(model);

    return true;
}

// Internal utility function:
// Creates a single mesh out of all the parts of the given model.
// This assumes that Mesh_GenFaces was already called on the parts of model.
void Hair::createHairMesh(const skeletal_model_s *model)
{
    m_mesh = new base_mesh_s();
    m_mesh->element_count_per_texture = (uint32_t *) calloc(sizeof(uint32_t), engine_world.tex_count);
    size_t totalElements = 0;

    // Gather size information
    for (int i = 0; i < model->mesh_count; i++)
    { const base_mesh_s *original = model->mesh_tree[i].mesh_base;

        m_mesh->num_texture_pages = std::max(m_mesh->num_texture_pages, original->num_texture_pages);

        for (int j = 0; j < original->num_texture_pages; j++) {
            m_mesh->element_count_per_texture[j] += original->element_count_per_texture[j];
            totalElements += original->element_count_per_texture[j];
        }
    }

    // Create arrays
    m_mesh->elements = (uint32_t *) calloc(sizeof(uint32_t), totalElements);

    // - with matrix index information
    m_mesh->matrix_indices = (int8_t *) calloc(sizeof(int8_t [2]), m_mesh->vertices.size());

    // Copy information
    std::vector<uint32_t> elementsStartPerTexture(m_mesh->num_texture_pages);
    m_mesh->vertices.clear();
    for (int i = 0; i < model->mesh_count; i++)
    {
        const base_mesh_s *original = model->mesh_tree[i].mesh_base;

        // Copy vertices
        const size_t verticesStart = m_mesh->vertices.size();
        m_mesh->vertices.insert(m_mesh->vertices.end(), original->vertices.begin(), original->vertices.end());

        // Copy elements
        uint32_t originalElementsStart = 0;
        for (int page = 0; page < original->num_texture_pages; page++)
        {
            memcpy(&m_mesh->elements[elementsStartPerTexture[page]],
                   &original->elements[originalElementsStart],
                   sizeof(uint32_t) * original->element_count_per_texture[page]);
            for (int j = 0; j < original->element_count_per_texture[page]; j++) {
                m_mesh->elements[elementsStartPerTexture[page]] = verticesStart + original->elements[originalElementsStart];
                originalElementsStart += 1;
                elementsStartPerTexture[page] += 1;
            }
        }

        /*
         * Apply total offset from parent.
         * The resulting mesh will have all the hair in default position
         * (i.e. as one big rope). The shader and matrix then transform it
         * correctly.
         */
        m_elements[i].position = model->mesh_tree[i].offset;
        if (i > 0)
        {
            // TODO: This assumes the parent is always the preceding mesh.
            // True for hair, obviously wrong for everything else. Can stay
            // here, but must go when we start generalizing the whole thing.
            m_elements[i].position += m_elements[i - 1].position;
        }

        // And create vertex data (including matrix indices)
        for (size_t j = 0; j < original->vertices.size(); j++) {
            if (original->vertices[j].position[1] <= 0)
            {
                m_mesh->matrix_indices[(verticesStart+j)*2 + 0] = i;
                m_mesh->matrix_indices[(verticesStart+j)*2 + 1] = i+1;
            }
            else
            {
                m_mesh->matrix_indices[(verticesStart+j)*2 + 0] = i+1;
                m_mesh->matrix_indices[(verticesStart+j)*2 + 1] = std::min((int8_t) (i+2), (int8_t) model->mesh_count);
            }

            // Now move all the hair vertices
            m_mesh->vertices[verticesStart+j].position += m_elements[i].position;

            // If the normal isn't fully in y direction, cancel its y component
            // This is perhaps a bit dubious.
            if (m_mesh->vertices[verticesStart+j].normal[0] != 0 || m_mesh->vertices[verticesStart+j].normal[2] != 0)
            {
                m_mesh->vertices[verticesStart+j].normal[1] = 0;
                m_mesh->vertices[verticesStart+j].normal.normalize();
            }
        }
    }

    Mesh_GenVBO(&renderer, m_mesh);
}

bool HairSetup::getSetup(uint32_t hair_entry_index)
{
    bool result = true;

    int top = lua_gettop(engine_lua);
    assert(top >= 0);

    lua_getglobal(engine_lua, "getHairSetup");
    if(lua_isfunction(engine_lua, -1))
    {
        lua_pushinteger(engine_lua, hair_entry_index);
        if(lua_CallAndLog(engine_lua, 1, 1, 0))
        {
            if(lua_istable(engine_lua, -1))
            {
                m_model               = (uint32_t)lua_GetScalarField(engine_lua, "model");
                m_linkBody           = (uint32_t)lua_GetScalarField(engine_lua, "link_body");

                lua_getfield(engine_lua, -1, "props");
                if(lua_istable(engine_lua, -1))
                {
                    m_rootWeight      = lua_GetScalarField(engine_lua, "root_weight");
                    m_tailWeight      = lua_GetScalarField(engine_lua, "tail_weight");
                    m_hairInertia     = lua_GetScalarField(engine_lua, "hair_inertia");
                    m_hairFriction    = lua_GetScalarField(engine_lua, "hair_friction");
                    m_hairRestitution = lua_GetScalarField(engine_lua, "hair_bouncing");
                    m_jointOverlap    = lua_GetScalarField(engine_lua, "joint_overlap");
                    m_jointCfm        = lua_GetScalarField(engine_lua, "joint_cfm");
                    m_jointErp        = lua_GetScalarField(engine_lua, "joint_erp");

                    lua_getfield(engine_lua, -1, "hair_damping");
                    if(lua_istable(engine_lua, -1))
                    {
                        m_hairDamping[0] = lua_GetScalarField(engine_lua, 1);
                        m_hairDamping[1] = lua_GetScalarField(engine_lua, 2);
                    }
                    lua_pop(engine_lua, 1);
                }
                else { result = false; }
                lua_pop(engine_lua, 1);

                lua_getfield(engine_lua, -1, "offset");
                if(lua_istable(engine_lua, -1))
                {
                    m_headOffset.m_floats[0] = lua_GetScalarField(engine_lua, 1);
                    m_headOffset.m_floats[1] = lua_GetScalarField(engine_lua, 2);
                    m_headOffset.m_floats[2] = lua_GetScalarField(engine_lua, 3);
                }
                else { result = false; }
                lua_pop(engine_lua, 1);

                lua_getfield(engine_lua, -1, "root_angle");
                if(lua_istable(engine_lua, -1))
                {
                    m_rootAngle[0] = lua_GetScalarField(engine_lua, 1);
                    m_rootAngle[1] = lua_GetScalarField(engine_lua, 2);
                    m_rootAngle[2] = lua_GetScalarField(engine_lua, 3);
                }
                else { result = false; }
                lua_pop(engine_lua, 1);
            }
            else { result = false; }
        }
        else { result = false; }
    }
    else { result = false; }

    lua_settop(engine_lua, top);
    return result;
}


Hair::~Hair()
{
    for(auto& joint : m_joints) {
        if(joint)
            bt_engine_dynamicsWorld->removeConstraint(joint.get());
    }

    for(auto& element : m_elements) {
        if(element.body) {
            element.body->setUserPointer(nullptr);
            bt_engine_dynamicsWorld->removeRigidBody(element.body.get());
        }
    }
}
