/*+-------------------------------------------------------------------------+
  |                       MultiVehicle simulator (libmvsim)                 |
  |                                                                         |
  | Copyright (C) 2014  Jose Luis Blanco Claraco (University of Almeria)    |
  | Distributed under GNU General Public License version 3                  |
  |   See <http://www.gnu.org/licenses/>                                    |
  +-------------------------------------------------------------------------+  */

#include <mvsim/World.h>
#include <mvsim/Block.h>

#include "JointXMLnode.h"
#include "XMLClassesRegistry.h"
#include "xml_utils.h"

#include <rapidxml.hpp>
#include <rapidxml_utils.hpp>
#include <rapidxml_print.hpp>
#include <mrpt/utils/utils_defs.h>  // mrpt::format()
#include <mrpt/poses/CPose2D.h>
#include <mrpt/opengl/CPolyhedron.h>

#include <sstream>      // std::stringstream
#include <map>
#include <string>

using namespace mvsim;
using namespace std;

XmlClassesRegistry block_classes_registry("block:class");

// Protected ctor:
Block::Block(World *parent) :
	VisualObject(parent),
	m_block_index(0),
	m_b2d_block_body(NULL),
	m_q(0,0,0,0,0,0),
	m_dq(0,0,0),
	m_mass(30.0),
	m_block_z_min(0.05),
	m_block_z_max(0.6),
	m_block_color(0x00,0x00,0xff),
	m_block_com(.0,.0),
	m_lateral_friction(0.5),
	m_ground_friction(0.5)
{
	using namespace mrpt::math;
	// Default shape:
	m_block_poly.push_back( TPoint2D(-0.5, -0.5) );
	m_block_poly.push_back( TPoint2D(-0.5,  0.5) );
	m_block_poly.push_back( TPoint2D( 0.5,  0.5) );
	m_block_poly.push_back( TPoint2D( 0.5, -0.5) );
	updateMaxRadiusFromPoly();

}

void Block::simul_post_timestep_common(const TSimulContext & /*context*/)
{
	if (m_b2d_block_body)
	{
		// Pos:
		const b2Vec2 &pos = m_b2d_block_body->GetPosition();
		const float32 angle = m_b2d_block_body->GetAngle();
		m_q.x=pos(0);
		m_q.y=pos(1);
		m_q.yaw=angle;
		// The rest (z,pitch,roll) will be always 0, unless other world-element modifies them! (e.g. elevation map)

		// Vel:
		const b2Vec2 &vel = m_b2d_block_body->GetLinearVelocity();
		const float32 w = m_b2d_block_body->GetAngularVelocity();
		m_dq.vals[0]=vel(0);
		m_dq.vals[1]=vel(1);
		m_dq.vals[2]=w;
	}
}


/** Register a new class of vehicles from XML description of type "<vehicle:class name='name'>...</vehicle:class>".  */
void Block::register_block_class(const rapidxml::xml_node<char> *xml_node)
{
	// Sanity checks:
	if (!xml_node) throw runtime_error("[Block::register_vehicle_class] XML node is NULL");
	if (0!=strcmp(xml_node->name(),"block:class")) throw runtime_error(mrpt::format("[Block::register_block_class] XML element is '%s' ('block:class' expected)",xml_node->name()));

	// rapidxml doesn't allow making copied of objects.
	// So: convert to txt; then re-parse.
	std::stringstream ss;
	ss << *xml_node;

	block_classes_registry.add( ss.str() );
}


/** Class factory: Creates a vehicle from XML description of type "<vehicle>...</vehicle>".  */
Block* Block::factory(World* parent, const rapidxml::xml_node<char> *root)
{
	using namespace std;
	using namespace rapidxml;

	if (!root) throw runtime_error("[Block::factory] XML node is NULL");
	if (0!=strcmp(root->name(),"block")) throw runtime_error(mrpt::format("[Block::factory] XML root element is '%s' ('block' expected)",root->name()));

	// "class": When there is a 'class="XXX"' attribute, look for each parameter
	//  in the set of "root" + "class_root" XML nodes:
	// --------------------------------------------------------------------------------
	JointXMLnode<> block_root_node;
	{
		block_root_node.add(root); // Always search in root. Also in the class root, if any:
		const xml_attribute<> *block_class = root->first_attribute("class");
		if (block_class)
		{
			const string sClassName = block_class->value();
			const rapidxml::xml_node<char>* class_root= block_classes_registry.get(sClassName);
			if (!class_root)
				throw runtime_error(mrpt::format("[Block::factory] Block class '%s' undefined",sClassName.c_str() ));
			block_root_node.add(class_root);
		}
	}

	// Build object (we don't use class factory for blocks)
	// ----------------------------------------------------
	Block *block = new Block(parent);

	// Init params
	// -------------------------------------------------
	// attrib: name
	{
		const xml_attribute<> *attrib_name = root->first_attribute("name");
		if (attrib_name && attrib_name->value())
		{
			block->m_name = attrib_name->value();
		}
		else
		{
			// Default name:
			static int cnt =0;
			block->m_name = mrpt::format("block%i",++cnt);
		}
	}

	// (Mandatory) initial pose:
	{
		const xml_node<> *node = block_root_node.first_node("init_pose");
		if (!node) throw runtime_error("[Block::factory] Missing XML node <init_pose>");

		if (3!= ::sscanf(node->value(),"%lf %lf %lf",&block->m_q.x,&block->m_q.y,&block->m_q.yaw))
			throw runtime_error("[Block::factory] Error parsing <init_pose>...</init_pose>");
		block->m_q.yaw *= M_PI/180.0; // deg->rad
	}

	// (Optional) initial vel:
	{
		const xml_node<> *node = block_root_node.first_node("init_vel");
		if (node)
		{
			if (3!= ::sscanf(node->value(),"%lf %lf %lf",&block->m_dq.vals[0],&block->m_dq.vals[1],&block->m_dq.vals[2]))
				throw runtime_error("[Block::factory] Error parsing <init_vel>...</init_vel>");
			block->m_dq.vals[2] *= M_PI/180.0; // deg->rad

			// Convert twist (velocity) from local -> global coords:
			const mrpt::poses::CPose2D pose(0,0,block->m_q.yaw); // Only the rotation
			pose.composePoint(
				block->m_dq.vals[0], block->m_dq.vals[1],
				block->m_dq.vals[0], block->m_dq.vals[1] );
		}
	}

	// Register bodies, fixtures, etc. in Box2D simulator:
	// ----------------------------------------------------
	b2World* b2world = parent->getBox2DWorld();
	block->create_multibody_system(b2world);

	if (block->m_b2d_block_body)
	{
		// Init pos:
		block->m_b2d_block_body->SetTransform( b2Vec2( block->m_q.x, block->m_q.y ),  block->m_q.yaw );
		// Init vel:
		block->m_b2d_block_body->SetLinearVelocity( b2Vec2(block->m_dq.vals[0], block->m_dq.vals[1] ) );
		block->m_b2d_block_body->SetAngularVelocity(block->m_dq.vals[2] );
	}

	return block;
}

Block* Block::factory(World* parent, const std::string &xml_text)
{
	// Parse the string as if it was an XML file:
	std::stringstream s;
	s.str( xml_text );

	char* input_str = const_cast<char*>(xml_text.c_str());
	rapidxml::xml_document<> xml;
	try {
		xml.parse<0>(input_str);
	}
	catch (rapidxml::parse_error &e) {
		unsigned int line = static_cast<long>(std::count(input_str, e.where<char>(), '\n') + 1);
		throw std::runtime_error( mrpt::format("[Block::factory] XML parse error (Line %u): %s", static_cast<unsigned>(line), e.what() ) );
	}
	return Block::factory(parent,xml.first_node());
}

void Block::simul_pre_timestep(const TSimulContext &context)
{
	// Apply motor forces/torques:
	//this->invoke_motor_controllers(context,m_torque_per_wheel);

	// Apply friction model:
	const double weight = m_mass * getWorldObject()->get_gravity();

	std::vector<mrpt::math::TSegment3D> force_vectors; // For visualization only
#if 0
	std::vector<mrpt::math::TPoint2D> wheels_vels;
	getWheelsVelocityLocal(wheels_vels,getVelocityLocal());

	for (size_t i=0;i<nW;i++)
	{
		// prepare data:
		Wheel &w = getWheelInfo(i);

		FrictionBase::TFrictionInput fi(context,w);
		fi.motor_torque = -m_torque_per_wheel[i];  // "-" => Forwards is negative
		fi.weight = weightPerWheel;
		fi.wheel_speed = wheels_vels[i];

		// eval friction:
		mrpt::math::TPoint2D net_force_;
		m_friction->evaluate_friction(fi,net_force_);

		// Apply force:
		const b2Vec2 wForce = m_b2d_block_body->GetWorldVector(b2Vec2(net_force_.x,net_force_.y)); // Force vector -> world coords
		const b2Vec2 wPt    = m_b2d_block_body->GetWorldPoint( b2Vec2( w.x, w.y) ); // Application point -> world coords
		//printf("w%i: Lx=%6.3f Ly=%6.3f  | Gx=%11.9f Gy=%11.9f\n",(int)i,net_force_.x,net_force_.y,wForce.x,wForce.y);

		m_b2d_block_body->ApplyForce( wForce,wPt, true/*wake up*/);

		// save it for optional rendering:
		if (m_world->m_gui_options.show_forces)
		{
			const double forceScale =  m_world->m_gui_options.force_scale; // [meters/N]
			const mrpt::math::TPoint3D pt1(wPt.x,wPt.y, m_chassis_z_max*1.1 + m_q.z );
			const mrpt::math::TPoint3D pt2 = pt1 + mrpt::math::TPoint3D(wForce.x,wForce.y, 0)*forceScale;
			force_vectors.push_back( mrpt::math::TSegment3D( pt1,pt2 ));
		}
	}
#endif

	// Save forces for optional rendering:
	if (m_world->m_gui_options.show_forces)
	{
		mrpt::synch::CCriticalSectionLocker csl( &m_force_segments_for_rendering_cs );
		m_force_segments_for_rendering = force_vectors;
	}
}

/** Override to do any required process right after the integration of dynamic equations for each timestep */
void Block::simul_post_timestep(const TSimulContext &context)
{
}

/** Last time-step velocity (of the ref. point, in local coords) */
vec3 Block::getVelocityLocal() const
{
	vec3 local_vel;
	local_vel.vals[2] = m_dq.vals[2]; // omega remains the same.

	const mrpt::poses::CPose2D p(0,0, -m_q.yaw); // "-" means inverse pose
	p.composePoint(
		m_dq.vals[0],m_dq.vals[1],
		local_vel.vals[0],local_vel.vals[1]);
	return local_vel;
}

mrpt::poses::CPose2D Block::getCPose2D() const
{
	return mrpt::poses::CPose2D(m_q);
}

/** To be called at derived classes' gui_update() */
void Block::gui_update( mrpt::opengl::COpenGLScene &scene)
{
	// 1st time call?? -> Create objects
	// ----------------------------------
	if (!m_gl_block)
	{
		m_gl_block = mrpt::opengl::CSetOfObjects::Create();

		// Block shape:
		mrpt::opengl::CPolyhedronPtr gl_poly = mrpt::opengl::CPolyhedron::CreateCustomPrism( m_block_poly, m_block_z_max-m_block_z_min);
		gl_poly->setLocation(0,0, m_block_z_min);
		gl_poly->setColor( mrpt::utils::TColorf(m_block_color) );
		m_gl_block->insert(gl_poly);

		SCENE_INSERT_Z_ORDER(scene,1, m_gl_block);

		// Visualization of forces:
		m_gl_forces = mrpt::opengl::CSetOfLines::Create();
		m_gl_forces->setLineWidth(3.0);
		m_gl_forces->setColor_u8(mrpt::utils::TColor(0xff,0xff,0xff));

		SCENE_INSERT_Z_ORDER(scene,3, m_gl_forces);  // forces are in global coords
	}

	// Update them:
	m_gl_block->setPose(m_q);

	// Other common stuff:
	internal_gui_update_forces(scene);
}

void Block::internal_gui_update_forces( mrpt::opengl::COpenGLScene &scene)
{
	if (m_world->m_gui_options.show_forces)
	{
		mrpt::synch::CCriticalSectionLocker csl( &m_force_segments_for_rendering_cs );
		m_gl_forces->clear();
		m_gl_forces->appendLines(m_force_segments_for_rendering);
		m_gl_forces->setVisibility(true);
	}
	else
	{
		m_gl_forces->setVisibility(false);
	}
}

void Block::updateMaxRadiusFromPoly()
{
	using namespace mrpt::math;

	m_max_radius=0.001f;
	for (TPolygon2D::const_iterator it=m_block_poly.begin();it!=m_block_poly.end();++it)
	{
		const float n=it->norm();
		mrpt::utils::keep_max(m_max_radius,n);
	}
}

/** Create bodies, fixtures, etc. for the dynamical simulation */
void Block::create_multibody_system(b2World* world)
{
	// Define the dynamic body. We set its position and call the body factory.
	b2BodyDef bodyDef;
	bodyDef.type = b2_dynamicBody;

	m_b2d_block_body = world->CreateBody(&bodyDef);

	// Define shape of block:
	// ------------------------------
	{
		// Convert shape into Box2D format:
		const size_t nPts = m_block_poly.size();
		ASSERT_(nPts>=3)
		ASSERT_BELOWEQ_(nPts, (size_t)b2_maxPolygonVertices)
		std::vector<b2Vec2> pts(nPts);
		for (size_t i=0;i<nPts;i++) pts[i] = b2Vec2( m_block_poly[i].x, m_block_poly[i].y );

		b2PolygonShape blockPoly;
		blockPoly.Set(&pts[0],nPts);
		//blockPoly.m_radius = 1e-3;  // The "skin" depth of the body

		// Define the dynamic body fixture.
		b2FixtureDef fixtureDef;
		fixtureDef.shape = &blockPoly;
		fixtureDef.restitution = 0.01;

		// Set the box density to be non-zero, so it will be dynamic.
		b2MassData mass;
		blockPoly.ComputeMass(&mass,1);  // Mass with density=1 => compute area
		fixtureDef.density = m_mass / mass.mass;

		// Override the default friction.
		fixtureDef.friction = m_lateral_friction; //0.3f;

		// Add the shape to the body.
		m_fixture_block = m_b2d_block_body->CreateFixture(&fixtureDef);

		// Compute center of mass:
		b2MassData vehMass;
		m_fixture_block->GetMassData(&vehMass);
		m_block_com.x = vehMass.center.x;
		m_block_com.y = vehMass.center.y;
	}
}

void Block::apply_force(double fx, double fy, double local_ptx, double local_pty)
{
	ASSERT_(m_b2d_block_body)
	const b2Vec2 wPt = m_b2d_block_body->GetWorldPoint( b2Vec2( local_ptx, local_pty) ); // Application point -> world coords
	m_b2d_block_body->ApplyForce( b2Vec2(fx,fy), wPt, true/*wake up*/);
}