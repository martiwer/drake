// clang-format: off
#include "drake/multibody/multibody_tree/multibody_tree.h"
// clang-format: on

#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include "drake/common/eigen_autodiff_types.h"
#include "drake/common/eigen_types.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"
#include "drake/multibody/benchmarks/acrobot/acrobot.h"
#include "drake/multibody/multibody_tree/fixed_offset_frame.h"
#include "drake/multibody/multibody_tree/revolute_mobilizer.h"
#include "drake/multibody/multibody_tree/rigid_body.h"
#include "drake/systems/framework/context.h"

namespace drake {
namespace multibody {
namespace {

const double kEpsilon = std::numeric_limits<double>::epsilon();

using benchmarks::Acrobot;
using Eigen::AngleAxisd;
using Eigen::Isometry3d;
using Eigen::Matrix2d;
using Eigen::Matrix3d;
using Eigen::Matrix4d;
using Eigen::Translation3d;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;
using std::make_unique;
using std::unique_ptr;
using std::vector;
using systems::Context;

// Set of MultibodyTree tests for a double pendulum model.
// This double pendulum is similar to the acrobot model described in Section 3.1
// of the Underactuated Robotics notes available online at
// http://underactuated.csail.mit.edu/underactuated.html?chapter=3.
// The only difference is that this model has no actuation.
// This double pendulum is defined in the x-y plane with gravity acting in the
// negative y-axis direction.
// In this model the two links of the pendulum have the same length with their
// respective centers of mass located at the links' centroids.
//
// The schematic below shows the location and relationship of the frames defined
// by the model. A few comments:
//  - The pendulum moves in the x-y plane, with angles θ₁ and θ₂ defined
//    positive according to the right-hand-rule with the thumb aligned in the
//    z-direction.
//  - The body frames for each link are placed at their geometric center.
//  - The origin of the shoulder frames (Si and So) are coincident at all times.
//    So is aligned with Si for θ₁ = 0.
//  - The origin of the elbow frames (Ei and Eo) are coincident at all times.
//    Eo is aligned with Ei for θ₂ = 0.
//
//       y ^
//         | Si ≡ W World body frame.
//         +--> x  Shoulder inboard frame Si coincides with W.
//      X_SiSo(θ₁) Shoulder revolute mobilizer with generalized position θ₁.
//      +--+-----+
//      |  ^     |
//      |  | So  | Shoulder outboard frame So.
//      |  +-->  |
//      |        |
//      |  X_USo | Pose of So in U.
//      |        |
//      |  ^     |
//      |  | U   | Upper link body frame U.
//      |  +-->  |
//      |        |
//      |  X_UEi | Pose of Ei in U.
//      |        |
//      |  ^     |
//      |  | Ei  | Elbow inboard frame Ei.
//      |  +-->  |
//      +--------+
//      X_EiEo(θ₂) Elbow revolute mobilizer with generalized position θ₂.
//      +--+-----+
//      |  ^     |
//      |  |Eo/L | Elbow outboard frame Eo.
//      |  +-->  | Lower link's frame L is coincident with the elbow frame Eo.
//      |        |
//      |p_LoLcm | Position vector of the link's com measured from the link's
//      |        | frame origin Lo.
//      |  ^     |
//      |  | Lcm | Lower link's frame L shifted to its center of mass.
//      |  +-->  |
//      |        |
//      |        |
//      |        |
//      |        |
//      |        |
//      |        |
//      +--------+
class PendulumTests : public ::testing::Test {
 public:
  // Creates an "empty" MultibodyTree that only contains the "world" body and
  // world body frame.
  void SetUp() override {
    model_ = std::make_unique<MultibodyTree<double>>();

    // Retrieves the world body.
    world_body_ = &model_->get_world_body();
  }

  // Sets up the MultibodyTree model for a double pendulum. See this unit test's
  // class description for details.
  void CreatePendulumModel() {
    // Spatial inertia of the upper link about its frame U and expressed in U.
    Vector3d link1_com_U = Vector3d::Zero();  // U is at the link's COM.
    // Inertia for a thin rod with moment of inertia link1_Ic_ about the y axis.
    UnitInertia<double> G_U =
        UnitInertia<double>::StraightLine(link1_Ic_, Vector3d::UnitY());
    SpatialInertia<double> M_U(link1_mass_, link1_com_U, G_U);

    // Spatial inertia of the lower link about its frame L and expressed in L.
    Vector3d link2_com_L = Vector3d::Zero();  // L is at the link's COM.
    // Inertia for a thin rod with moment of inertia link2_Ic_ about the y axis.
    UnitInertia<double> G_Lcm =
        UnitInertia<double>::StraightLine(link2_Ic_, Vector3d::UnitY());
    // Spatial inertia about L's center of mass Lcm.
    SpatialInertia<double> M_Lcm(link2_mass_, link2_com_L, G_Lcm);
    // Since L's frame origin Lo is not at the the lower link's center of mass
    // Lcm, we must shift M_Lcm to obtain M_Lo.
    const Vector3d p_LoLcm(0.0, -half_link2_length_, 0.0);
    SpatialInertia<double> M_L = M_Lcm.Shift(-p_LoLcm);

    // Adds the upper and lower links of the pendulum.
    // Using: const BodyType& AddBody(std::unique_ptr<BodyType> body).
    upper_link_ =
        &model_->AddBody(make_unique<RigidBody<double>>(M_U));
    // Using: const BodyType<T>& AddBody(Args&&... args)
    lower_link_ = &model_->AddBody<RigidBody>(M_L);

    // The shoulder is the mobilizer that connects the world to the upper link.
    // Its inboard frame, Si, is the world frame. Its outboard frame, So, a
    // fixed offset frame on the upper link.
    shoulder_inboard_frame_ = &model_->get_world_frame();

    // The body frame of the upper link is U, and that of the lower link is L.
    // We will add a frame for the pendulum's shoulder. This will be the
    // shoulder's outboard frame So.
    // X_USo specifies the pose of the shoulder outboard frame So in the body
    // frame U of the upper link.
    // In this case the frame is created explicitly from the body frame of
    // upper_link.
    shoulder_outboard_frame_ =
        &model_->AddFrame<FixedOffsetFrame>(
            upper_link_->get_body_frame(), X_USo_);

    // The elbow is the mobilizer that connects upper and lower links.
    // Below we will create inboard and outboard frames associated with the
    // pendulum's elbow.
    // An inboard frame Ei is rigidly attached to the upper link. It is located
    // at y = -half_link_length_ in the frame of the upper link body.
    // X_UEi specifies the pose of the elbow inboard frame Ei in the body
    // frame U of the upper link.
    // In this case we create a frame using the FixedOffsetFrame::Create()
    // method taking a Body, i.e., creating a frame with a fixed offset from the
    // upper link body frame.
    elbow_inboard_frame_ =
        &model_->AddFrame<FixedOffsetFrame>(*upper_link_, X_UEi_);

    // To make this test a bit more interesting, we define the lower link's
    // frame L to be coincident with the elbow's outboard frame. Therefore,
    // Lo != Lcm.
    elbow_outboard_frame_ = &lower_link_->get_body_frame();

    // Adds the shoulder and elbow mobilizers of the pendulum.
    // Using:
    //  const Mobilizer& AddMobilizer(std::unique_ptr<MobilizerType> mobilizer).
    shoulder_mobilizer_ =
        &model_->AddMobilizer(
            make_unique<RevoluteMobilizer<double>>(
                *shoulder_inboard_frame_, *shoulder_outboard_frame_,
                Vector3d::UnitZ() /*revolute axis*/));
    // Using: const MobilizerType<T>& AddMobilizer(Args&&... args)
    elbow_mobilizer_ = &model_->AddMobilizer<RevoluteMobilizer>(
        *elbow_inboard_frame_, *elbow_outboard_frame_,
        Vector3d::UnitZ() /*revolute axis*/);
  }

  // Helper method to extract a pose from the position kinematics.
  // TODO(amcastro-tri):
  // Replace this by a method Body<T>::get_pose_in_world(const Context<T>&)
  // when we can place cache entries in the context.
  template <typename T>
  const Isometry3<T>& get_body_pose_in_world(
      const PositionKinematicsCache<T>& pc,
      const Body<T>& body) const {
    const MultibodyTreeTopology& topology = model_->get_topology();
    // Cache entries are accessed by BodyNodeIndex for fast traversals.
    return pc.get_X_WB(topology.get_body(body.get_index()).body_node);
  }

  // Helper method to extract spatial velocity from the velocity kinematics
  // cache.
  // TODO(amcastro-tri):
  // Replace this by a method
  // Body<T>::get_spatial_velocity_in_world(const Context<T>&)
  // when we can place cache entries in the context.
  const SpatialVelocity<double>& get_body_spatial_velocity_in_world(
      const VelocityKinematicsCache<double>& vc,
      const Body<double>& body) const {
    const MultibodyTreeTopology& topology = model_->get_topology();
    // Cache entries are accessed by BodyNodeIndex for fast traversals.
    return vc.get_V_WB(topology.get_body(body.get_index()).body_node);
  }

  // Helper method to extract spatial acceleration from the acceleration
  // kinematics cache.
  // TODO(amcastro-tri):
  // Replace this by a method
  // Body<T>::get_spatial_acceleration_in_world(const Context<T>&)
  // when we can place cache entries in the context.
  const SpatialAcceleration<double>& get_body_spatial_acceleration_in_world(
      const AccelerationKinematicsCache<double>& ac,
      const Body<double>& body) const {
    const MultibodyTreeTopology& topology = model_->get_topology();
    // Cache entries are accessed by BodyNodeIndex for fast traversals.
    return ac.get_A_WB(topology.get_body(body.get_index()).body_node);
  }

 protected:
  // For testing only so that we can retrieve/set (future to be) cache entries,
  // this method initializes the poses of each link in the position kinematics
  // cache.
  void SetPendulumPoses(PositionKinematicsCache<double>* pc) {
    pc->get_mutable_X_WB(BodyNodeIndex(1)) = X_WL_;
  }

  std::unique_ptr<MultibodyTree<double>> model_;
  const Body<double>* world_body_;
  // Bodies:
  const RigidBody<double>* upper_link_;
  const RigidBody<double>* lower_link_;
  // Frames:
  const BodyFrame<double>* shoulder_inboard_frame_;
  const FixedOffsetFrame<double>* shoulder_outboard_frame_;
  const FixedOffsetFrame<double>* elbow_inboard_frame_;
  const Frame<double>* elbow_outboard_frame_;
  // Mobilizers:
  const RevoluteMobilizer<double>* shoulder_mobilizer_;
  const RevoluteMobilizer<double>* elbow_mobilizer_;
  // Pendulum parameters:
  const double link1_length_ = 1.0;
  const double link1_mass_ = 1.0;
  // Unit inertia about an axis perpendicular to the rod for link1.
  const double link1_Ic_ = .083;
  const double link2_length_ = 2.0;
  const double link2_mass_ = 1.0;
  // Unit inertia about an axis perpendicular to the rod for link2.
  const double link2_Ic_ = .33;
  const double half_link1_length_ = link1_length_ / 2;
  const double half_link2_length_ = link2_length_ / 2;
  // Acceleration of gravity at Earth's surface.
  const double acceleration_of_gravity_ = 9.81;
  // Poses:
  // Desired pose of the lower link frame L in the world frame W.
  const Isometry3d X_WL_{Translation3d(0.0, -half_link1_length_, 0.0)};
  // Pose of the shoulder outboard frame So in the upper link frame U.
  const Isometry3d X_USo_{Translation3d(0.0, half_link1_length_, 0.0)};
  // Pose of the elbow inboard frame Ei in the upper link frame U.
  const Isometry3d X_UEi_{Translation3d(0.0, -half_link1_length_, 0.0)};
  // Pose of the elbow outboard frame Eo in the lower link frame L.
  const Isometry3d X_LEo_{Translation3d(0.0, half_link2_length_, 0.0)};
};

TEST_F(PendulumTests, CreateModelBasics) {
  // Initially there is only one body, the world.
  EXPECT_EQ(model_->get_num_bodies(), 1);
  // And there is only one frame, the world frame.
  EXPECT_EQ(model_->get_num_frames(), 1);

  CreatePendulumModel();

  // Verifies the number of multibody elements is correct.
  EXPECT_EQ(model_->get_num_bodies(), 3);
  EXPECT_EQ(model_->get_num_frames(), 5);
  EXPECT_EQ(model_->get_num_mobilizers(), 2);

  // Check that frames are associated with the correct bodies.
  EXPECT_EQ(
      shoulder_inboard_frame_->get_body().get_index(),
      world_body_->get_index());
  EXPECT_EQ(
      shoulder_outboard_frame_->get_body().get_index(),
      upper_link_->get_index());
  EXPECT_EQ(
      elbow_inboard_frame_->get_body().get_index(), upper_link_->get_index());
  EXPECT_EQ(
      elbow_outboard_frame_->get_body().get_index(), lower_link_->get_index());

  // Checks that mobilizers connect the right frames.
  EXPECT_EQ(shoulder_mobilizer_->get_inboard_frame().get_index(),
            world_body_->get_body_frame().get_index());
  EXPECT_EQ(shoulder_mobilizer_->get_outboard_frame().get_index(),
            shoulder_outboard_frame_->get_index());
  EXPECT_EQ(elbow_mobilizer_->get_inboard_frame().get_index(),
            elbow_inboard_frame_->get_index());
  EXPECT_EQ(elbow_mobilizer_->get_outboard_frame().get_index(),
            elbow_outboard_frame_->get_index());

  // Checks that mobilizers connect the right bodies.
  EXPECT_EQ(shoulder_mobilizer_->get_inboard_body().get_index(),
            world_body_->get_index());
  EXPECT_EQ(shoulder_mobilizer_->get_outboard_body().get_index(),
            upper_link_->get_index());
  EXPECT_EQ(elbow_mobilizer_->get_inboard_body().get_index(),
            upper_link_->get_index());
  EXPECT_EQ(elbow_mobilizer_->get_outboard_body().get_index(),
            lower_link_->get_index());

  // Checks we can retrieve the body associated with a frame.
  EXPECT_EQ(&shoulder_inboard_frame_->get_body(), world_body_);
  EXPECT_EQ(&shoulder_outboard_frame_->get_body(), upper_link_);
  EXPECT_EQ(&elbow_inboard_frame_->get_body(), upper_link_);
  EXPECT_EQ(&elbow_outboard_frame_->get_body(), lower_link_);

  // Checks we can request inboard/outboard bodies to a mobilizer.
  EXPECT_EQ(&shoulder_mobilizer_->get_inboard_body(), world_body_);
  EXPECT_EQ(&shoulder_mobilizer_->get_outboard_body(), upper_link_);
  EXPECT_EQ(&elbow_mobilizer_->get_inboard_body(), upper_link_);
  EXPECT_EQ(&elbow_mobilizer_->get_outboard_body(), lower_link_);

  // Request revolute mobilizers' axes.
  EXPECT_EQ(shoulder_mobilizer_->get_revolute_axis(), Vector3d::UnitZ());
  EXPECT_EQ(elbow_mobilizer_->get_revolute_axis(), Vector3d::UnitZ());
}

// Frame indexes are assigned by MultibodyTree. The number of frames
// equals the number of body frames (one per body) plus the number of
// additional frames added to the system (like FixedOffsetFrame objects).
// Frames are indexed in the order they are added to the MultibodyTree model.
// The order of the frames and their indexes is an implementation detail that
// users do not need to know about. Therefore this unit test would need to
// change in the future if we decide to change the "internal detail" on how we
// assign these indexes.
TEST_F(PendulumTests, Indexes) {
  CreatePendulumModel();
  EXPECT_EQ(shoulder_inboard_frame_->get_index(), FrameIndex(0));
  EXPECT_EQ(upper_link_->get_body_frame().get_index(), FrameIndex(1));
  EXPECT_EQ(lower_link_->get_body_frame().get_index(), FrameIndex(2));
  EXPECT_EQ(shoulder_outboard_frame_->get_index(), FrameIndex(3));
  EXPECT_EQ(elbow_inboard_frame_->get_index(), FrameIndex(4));
  EXPECT_EQ(elbow_outboard_frame_->get_index(), FrameIndex(2));
}

// Asserts that the Finalize() stage is successful and that re-finalization is
// not allowed.
TEST_F(PendulumTests, Finalize) {
  CreatePendulumModel();
  // Finalize() stage.
  EXPECT_FALSE(model_->topology_is_valid());  // Not valid before Finalize().
  EXPECT_NO_THROW(model_->Finalize());
  EXPECT_TRUE(model_->topology_is_valid());  // Valid after Finalize().

  // Asserts that no more multibody elements can be added after finalize.
  SpatialInertia<double> M_Bo_B;
  EXPECT_THROW(model_->AddBody<RigidBody>(M_Bo_B), std::logic_error);
  EXPECT_THROW(model_->AddFrame<FixedOffsetFrame>(*lower_link_, X_LEo_),
               std::logic_error);
  EXPECT_THROW(model_->AddMobilizer<RevoluteMobilizer>(
      *shoulder_inboard_frame_, *shoulder_outboard_frame_,
      Vector3d::UnitZ()), std::logic_error);

  // Asserts re-finalization is not allowed.
  EXPECT_THROW(model_->Finalize(), std::logic_error);
}

// This is an experiment with std::reference_wrapper to show that we can save
// bodies in an array of references.
TEST_F(PendulumTests, StdReferenceWrapperExperiment) {
  // Initially there is only one body, the world.
  EXPECT_EQ(model_->get_num_bodies(), 1);
  // And there is only one frame, the world frame.
  EXPECT_EQ(model_->get_num_frames(), 1);
  CreatePendulumModel();

  // Vector of references.
  vector<std::reference_wrapper<const Body<double>>> bodies;
  bodies.push_back(*world_body_);
  bodies.push_back(*upper_link_);
  bodies.push_back(*lower_link_);

  // Verify that vector "bodies" effectively holds valid references to the
  // actual body elements in the tree.
  // In addition, since these tests compare actual memory addresses, they
  // ensure that bodies were not copied instead.
  // Unfortunately we need the ugly get() method since operator.() is not
  // overloaded.
  EXPECT_EQ(&bodies[world_body_->get_index()].get(), world_body_);
  EXPECT_EQ(&bodies[upper_link_->get_index()].get(), upper_link_);
  EXPECT_EQ(&bodies[lower_link_->get_index()].get(), lower_link_);
}

TEST_F(PendulumTests, CreateContext) {
  CreatePendulumModel();

  // Verifies the number of multibody elements is correct. In this case:
  // - world_
  // - upper_link_
  // - lower_link_
  EXPECT_EQ(model_->get_num_bodies(), 3);

  // Verify we cannot create a Context until we have a valid topology.
  EXPECT_FALSE(model_->topology_is_valid());  // Not valid before Finalize().
  EXPECT_THROW(model_->CreateDefaultContext(), std::logic_error);

  // Finalize() stage.
  EXPECT_NO_THROW(model_->Finalize());
  EXPECT_TRUE(model_->topology_is_valid());  // Valid after Finalize().

  // Create Context.
  std::unique_ptr<Context<double>> context;
  EXPECT_NO_THROW(context = model_->CreateDefaultContext());

  // Tests MultibodyTreeContext accessors.
  auto mbt_context =
      dynamic_cast<MultibodyTreeContext<double>*>(context.get());
  ASSERT_TRUE(mbt_context != nullptr);

  // Verifies the correct number of generalized positions and velocities.
  EXPECT_EQ(mbt_context->get_positions().size(), 2);
  EXPECT_EQ(mbt_context->get_mutable_positions().size(), 2);
  EXPECT_EQ(mbt_context->get_velocities().size(), 2);
  EXPECT_EQ(mbt_context->get_mutable_velocities().size(), 2);

  // Verifies methods to retrieve fixed-sized segments of the state.
  EXPECT_EQ(mbt_context->get_state_segment<1>(1).size(), 1);
  EXPECT_EQ(mbt_context->get_mutable_state_segment<1>(1).size(), 1);

  // Set the poses of each body in the position kinematics cache to have an
  // arbitrary value that we can use for unit testing. In practice the poses in
  // the position kinematics will be the result of a position kinematics update
  // and will live in the context as a cache entry.
  PositionKinematicsCache<double> pc(model_->get_topology());
  SetPendulumPoses(&pc);

  // Retrieve body poses from position kinematics cache.
  const Isometry3d &X_WW = get_body_pose_in_world(pc, *world_body_);
  const Isometry3d &X_WLu = get_body_pose_in_world(pc, *upper_link_);

  // Asserts that the retrieved poses match with the ones specified by the unit
  // test method SetPendulumPoses().
  EXPECT_TRUE(X_WW.matrix().isApprox(Matrix4d::Identity()));
  EXPECT_TRUE(X_WLu.matrix().isApprox(X_WL_.matrix()));
}

// Unit test fixture to verify the correctness of MultibodyTree methods for
// computing kinematics. This fixture uses the reference solution provided by
// benchmarks::Acrobot.
class PendulumKinematicTests : public PendulumTests {
 public:
  void SetUp() override {
    PendulumTests::SetUp();
    CreatePendulumModel();
    model_->Finalize();
    context_ = model_->CreateDefaultContext();
    mbt_context_ =
        dynamic_cast<MultibodyTreeContext<double>*>(context_.get());
  }

  /// Verifies that we can compute the mass matrix of the system using inverse
  /// dynamics.
  /// The result from inverse dynamics is the vector of generalized forces:
  ///   tau = M(q) * vdot + C(q, v) * v
  /// where q and v are the generalized positions and velocities, respectively.
  /// When v = 0 the Coriolis and gyroscopic forces term C(q, v) * v is zero.
  /// Therefore the i-th column of M(q) can be obtained performing inverse
  /// dynamics with an acceleration vector vdot = e_i, with e_i the i-th vector
  /// in the standard basis of ℝ²:
  ///   tau = Hi(q) = M(q) * e_i
  /// where Hi(q) is the i-th column in M(q).
  ///
  /// The solution is verified against the independent benchmark from
  /// drake::multibody::benchmarks::Acrobot.
  void VerifyMassMatrixViaInverseDynamics(
      double shoulder_angle, double elbow_angle) {
    shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
    elbow_mobilizer_->set_angle(context_.get(), elbow_angle);

    Matrix2d H;
    model_->CalcMassMatrixViaInverseDynamics(*context_, &H);

    Matrix2d H_expected = acrobot_benchmark_.CalcMassMatrix(elbow_angle);
    EXPECT_TRUE(H.isApprox(H_expected, 5 * kEpsilon));
  }

  /// Verifies the results from MultibodyTree::CalcInverseDynamics() for a
  /// number of state configurations against the independently coded
  /// implementation in drake::multibody::benchmarks::Acrobot.
  void VerifyCoriolisTermViaInverseDynamics(
      double shoulder_angle, double elbow_angle) {
    const double kTolerance = 5 * kEpsilon;

    shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
    elbow_mobilizer_->set_angle(context_.get(), elbow_angle);

    double shoulder_rate, elbow_rate;
    Vector2d C;
    Vector2d C_expected;

    // C(q, v) = 0 for v = 0.
    shoulder_rate = 0.0;
    elbow_rate = 0.0;
    shoulder_mobilizer_->set_angular_rate(context_.get(), shoulder_rate);
    elbow_mobilizer_->set_angular_rate(context_.get(), elbow_rate);
    model_->CalcBiasTerm(*context_, &C);
    C_expected = acrobot_benchmark_.CalcCoriolisVector(
            shoulder_angle, elbow_angle, shoulder_rate, elbow_rate);
    EXPECT_TRUE(CompareMatrices(
        C, C_expected, kTolerance, MatrixCompareType::relative));

    // First column of C(q, e_1) times e_1.
    shoulder_rate = 1.0;
    elbow_rate = 0.0;
    shoulder_mobilizer_->set_angular_rate(context_.get(), shoulder_rate);
    elbow_mobilizer_->set_angular_rate(context_.get(), elbow_rate);
    model_->CalcBiasTerm(*context_, &C);
    C_expected = acrobot_benchmark_.CalcCoriolisVector(
        shoulder_angle, elbow_angle, shoulder_rate, elbow_rate);
    EXPECT_TRUE(CompareMatrices(
        C, C_expected, kTolerance, MatrixCompareType::relative));

    // Second column of C(q, e_2) times e_2.
    shoulder_rate = 0.0;
    elbow_rate = 1.0;
    shoulder_mobilizer_->set_angular_rate(context_.get(), shoulder_rate);
    elbow_mobilizer_->set_angular_rate(context_.get(), elbow_rate);
    model_->CalcBiasTerm(*context_, &C);
    C_expected = acrobot_benchmark_.CalcCoriolisVector(
        shoulder_angle, elbow_angle, shoulder_rate, elbow_rate);
    EXPECT_TRUE(CompareMatrices(
        C, C_expected, kTolerance, MatrixCompareType::relative));

    // Both velocities are non-zero.
    shoulder_rate = 1.0;
    elbow_rate = 1.0;
    shoulder_mobilizer_->set_angular_rate(context_.get(), shoulder_rate);
    elbow_mobilizer_->set_angular_rate(context_.get(), elbow_rate);
    model_->CalcBiasTerm(*context_, &C);
    C_expected = acrobot_benchmark_.CalcCoriolisVector(
        shoulder_angle, elbow_angle, shoulder_rate, elbow_rate);
    EXPECT_TRUE(CompareMatrices(
        C, C_expected, kTolerance, MatrixCompareType::relative));
  }

  /// This method verifies the correctness of
  /// MultibodyTree::CalcForceElementsContribution() to compute the vector of
  /// generalized forces due to gravity.
  /// Generalized forces due to gravity are a function of positions only and are
  /// denoted by G(q).
  /// The solution is verified against the independent benchmark from
  /// drake::multibody::benchmarks::Acrobot.
  Vector2d VerifyGravityTerm(
      const Eigen::Ref<const VectorXd>& q) const {
    DRAKE_DEMAND(q.size() == model_->get_num_positions());

    // This is the minimum factor of the machine precision within which these
    // tests pass. This factor incorporates an additional factor of two (2) to
    // be on the safe side on other architectures (particularly in Macs).
    const int kEpsilonFactor = 5;
    const double kTolerance = kEpsilonFactor * kEpsilon;

    const double shoulder_angle =  q(0);
    const double elbow_angle =  q(1);

    PositionKinematicsCache<double> pc(model_->get_topology());
    VelocityKinematicsCache<double> vc(model_->get_topology());
    // Even though G(q) only depends on positions, other velocity dependent
    // forces (for instance damping) could depend on velocities. Therefore we
    // set the velocity kinematics cache entries to zero so that only G(q) gets
    // computed (at least for this pendulum model that only includes gravity
    // and damping).
    vc.InitializeToZero();

    // ======================================================================
    // Compute position kinematics.
    shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
    elbow_mobilizer_->set_angle(context_.get(), elbow_angle);
    model_->CalcPositionKinematicsCache(*context_, &pc);

    // ======================================================================
    // Compute inverse dynamics. Add applied forces due to gravity.

    // Spatial force on the upper link due to gravity.
    const SpatialForce<double> F_U_W =
        SpatialForce<double>(
            Vector3d::Zero(),
            -link1_mass_ * acceleration_of_gravity_ * Vector3d::UnitY());

    // Spatial force on the lower link due to gravity.
    const SpatialForce<double> F_Lcm_W =
        SpatialForce<double>(
            Vector3d::Zero(),
            -link2_mass_ * acceleration_of_gravity_ * Vector3d::UnitY());
    // Obtain the position of the lower link's center of mass.
    const Isometry3d& X_WL = get_body_pose_in_world(pc, *lower_link_);
    const Matrix3d R_WL = X_WL.linear();
    const Vector3d p_LoLcm_L = lower_link_->get_default_com();
    const Vector3d p_LoLcm_W = R_WL * p_LoLcm_L;
    const SpatialForce<double> F_L_W = F_Lcm_W.Shift(-p_LoLcm_W);

    // Output vector of generalized forces.
    VectorXd tau(model_->get_num_velocities());
    // Input vector of applied generalized forces.
    VectorXd tau_applied(model_->get_num_velocities());

    vector<SpatialForce<double>> F_Bo_W_array(model_->get_num_bodies());
    F_Bo_W_array[upper_link_->get_node_index()] = F_U_W;
    F_Bo_W_array[lower_link_->get_node_index()] = F_L_W;

    // Output vector of spatial forces for each body B at their inboard
    // frame Mo, expressed in the world W.
    vector<SpatialForce<double>> F_BMo_W_array(model_->get_num_bodies());

    // ======================================================================
    // Compute expected values using the acrobot benchmark.
    const Vector2d G_expected = acrobot_benchmark_.CalcGravityVector(
        shoulder_angle, elbow_angle);

    // ======================================================================
    // Notice that we do not need to allocate extra memory since both
    // F_Bo_W_array and tau can be used as input and output arguments. However,
    // the data given at input is lost on output. A user might choose then to
    // have separate input/output arrays.
    const VectorXd vdot = VectorXd::Zero(model_->get_num_velocities());
    vector<SpatialAcceleration<double>> A_WB_array(model_->get_num_bodies());

    // Try first using different arrays for input/ouput:
    // Initialize output to garbage, it should not affect the results.
    tau.setConstant(std::numeric_limits<double>::quiet_NaN());
    tau_applied.setZero();
    model_->CalcInverseDynamics(
        *context_, pc, vc, vdot, F_Bo_W_array, tau_applied,
        &A_WB_array, &F_BMo_W_array, &tau);
    EXPECT_TRUE(tau.isApprox(G_expected, kTolerance));

    // Now try using the same arrays for input/output (input data F_Bo_W_array
    // will get overwritten through the output argument).
    tau_applied.setZero();  // This will now get overwritten.
    model_->CalcInverseDynamics(
        *context_, pc, vc, vdot, F_Bo_W_array, tau_applied,
        &A_WB_array, &F_Bo_W_array, &tau_applied);
    EXPECT_TRUE(tau.isApprox(G_expected, kTolerance));
    return tau;
  }

  /// Given the transformation `X_AB` between two frames A and B and its time
  /// derivative in frame A `Xdot_AB`, this method computes the spatial velocity
  /// `V_AB` of frame B as measured and expressed in A.
  static SpatialVelocity<double> ComputeSpatialVelocityFromXdot(
      const Matrix4d& X_AB, const Matrix4d& X_AB_dot) {
    const Matrix3d R_AB = X_AB.topLeftCorner(3, 3);
    const Matrix3d R_AB_dot = X_AB_dot.topLeftCorner(3, 3);
    // Compute cross product matrix w_ABx = [w_AB].
    Matrix3d w_ABx = R_AB_dot * R_AB.transpose();
    // Take the average to take into account both upper and lower parts.
    w_ABx = (w_ABx - w_ABx.transpose()) / 2.0;
    // Extract angular velocity vector.
    Vector3d w_AB(w_ABx(2, 1), w_ABx(0, 2), w_ABx(1, 0));
    // Extract linear velocity vector.
    Vector3d v_AB = X_AB_dot.col(3).head(3);
    return SpatialVelocity<double>(w_AB, v_AB);
  }

 protected:
  std::unique_ptr<Context<double>> context_;
  MultibodyTreeContext<double>* mbt_context_;
  // Reference benchmark for verification.
  Acrobot<double> acrobot_benchmark_{
      Vector3d::UnitZ() /* Plane normal */, Vector3d::UnitY() /* Up vector */,
      link1_mass_, link2_mass_,
      link1_length_, link2_length_, half_link1_length_, half_link2_length_,
      link1_Ic_, link2_Ic_};

 private:
  // This method verifies the correctness of
  // MultibodyTree::CalcInverseDynamics() to compute the generalized forces that
  // would need to be applied in order to attain the generalized accelerations
  // vdot.
  // The generalized accelerations are given by:
  //   tau = M(q) * vdot + C(q, v) * v
  // where q and v are the generalized positions and velocities, respectively.
  // These, together with the generalized accelerations vdot are inputs to this
  // method.
  // The solution is verified against the independent benchmark from
  // drake::multibody::benchmarks::Acrobot.
  Vector2d VerifyInverseDynamics(
      const Eigen::Ref<const VectorXd>& q,
      const Eigen::Ref<const VectorXd>& v,
      const Eigen::Ref<const VectorXd>& vdot) const {
    DRAKE_DEMAND(q.size() == model_->get_num_positions());
    DRAKE_DEMAND(v.size() == model_->get_num_velocities());
    DRAKE_DEMAND(vdot.size() == model_->get_num_velocities());

    // This is the minimum factor of the machine precision within which these
    // tests pass. This factor incorporates an additional factor of two (2) to
    // be on the safe side on other architectures (particularly in Macs).
    const int kEpsilonFactor = 30;
    const double kTolerance = kEpsilonFactor * kEpsilon;

    const double shoulder_angle =  q(0);
    const double elbow_angle =  q(1);

    const double shoulder_angle_rate = v(0);
    const double elbow_angle_rate = v(1);

    PositionKinematicsCache<double> pc(model_->get_topology());
    VelocityKinematicsCache<double> vc(model_->get_topology());

    // ======================================================================
    // Compute position kinematics.
    shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
    elbow_mobilizer_->set_angle(context_.get(), elbow_angle);
    model_->CalcPositionKinematicsCache(*context_, &pc);

    // ======================================================================
    // Compute velocity kinematics.
    shoulder_mobilizer_->set_angular_rate(context_.get(), shoulder_angle_rate);
    elbow_mobilizer_->set_angular_rate(context_.get(), elbow_angle_rate);
    model_->CalcVelocityKinematicsCache(*context_, pc, &vc);

    // ======================================================================
    // Compute inverse dynamics.
    VectorXd tau(model_->get_num_velocities());
    vector<SpatialAcceleration<double>> A_WB_array(model_->get_num_bodies());
    vector<SpatialForce<double>> F_BMo_W_array(model_->get_num_bodies());
    model_->CalcInverseDynamics(*context_, pc, vc, vdot, {}, VectorXd(),
                                &A_WB_array, &F_BMo_W_array, &tau);

    // ======================================================================
    // Compute acceleration kinematics.
    AccelerationKinematicsCache<double> ac(model_->get_topology());
    model_->CalcAccelerationKinematicsCache(*context_, pc, vc, vdot, &ac);

    // From acceleration kinematics.
    const SpatialAcceleration<double>& A_WUcm_ac =
        get_body_spatial_acceleration_in_world(ac, *upper_link_);
    const SpatialAcceleration<double>& A_WL_ac =
        get_body_spatial_acceleration_in_world(ac, *lower_link_);
    // From inverse dynamics.
    const SpatialAcceleration<double>& A_WUcm_id =
        A_WB_array[upper_link_->get_node_index()];
    const SpatialAcceleration<double>& A_WL_id =
        A_WB_array[lower_link_->get_node_index()];
    EXPECT_TRUE(A_WUcm_id.IsApprox(A_WUcm_ac, kTolerance));
    EXPECT_TRUE(A_WL_id.IsApprox(A_WL_ac, kTolerance));

    // ======================================================================
    // Compute expected values using the acrobot benchmark.
    const Vector2d C_expected = acrobot_benchmark_.CalcCoriolisVector(
        shoulder_angle, elbow_angle, shoulder_angle_rate, elbow_angle_rate);
    const Matrix2d H = acrobot_benchmark_.CalcMassMatrix(elbow_angle);
    const Vector2d tau_expected = H * vdot + C_expected;

    EXPECT_TRUE(CompareMatrices(tau, tau_expected, kTolerance,
                                MatrixCompareType::relative));
    return tau;
  }
};

// Verify the correctness of method MultibodyTree::CalcPositionKinematicsCache()
// comparing the computed results the reference solution provided by
// benchmarks::Acrobot.
TEST_F(PendulumKinematicTests, CalcPositionKinematics) {
  // This is the minimum factor of the machine precision within which these
  // tests pass.
  const int kEpsilonFactor = 3;
  const double kTolerance = kEpsilonFactor * kEpsilon;

  // By default CreateDefaultContext() sets mobilizer to their zero
  // configuration.
  EXPECT_EQ(shoulder_mobilizer_->get_angle(*context_), 0.0);
  EXPECT_EQ(elbow_mobilizer_->get_angle(*context_), 0.0);

  // Test mobilizer's setter/getters.
  shoulder_mobilizer_->set_angle(context_.get(), M_PI);
  EXPECT_EQ(shoulder_mobilizer_->get_angle(*context_), M_PI);
  shoulder_mobilizer_->set_zero_configuration(context_.get());
  EXPECT_EQ(shoulder_mobilizer_->get_angle(*context_), 0.0);

  PositionKinematicsCache<double> pc(model_->get_topology());

  const int num_angles = 50;
  const double kDeltaAngle = 2 * M_PI / (num_angles - 1.0);
  for (double ishoulder = 0; ishoulder < num_angles; ++ishoulder) {
    const double shoulder_angle = -M_PI + ishoulder * kDeltaAngle;
    for (double ielbow = 0; ielbow < num_angles; ++ielbow) {
      const double elbow_angle = -M_PI + ielbow * kDeltaAngle;

      shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
      EXPECT_EQ(shoulder_mobilizer_->get_angle(*context_), shoulder_angle);
      elbow_mobilizer_->set_angle(context_.get(), elbow_angle);
      EXPECT_EQ(elbow_mobilizer_->get_angle(*context_), elbow_angle);

      // Verify this matches the corresponding entries in the context.
      EXPECT_EQ(mbt_context_->get_positions()(0), shoulder_angle);
      EXPECT_EQ(mbt_context_->get_positions()(1), elbow_angle);

      model_->CalcPositionKinematicsCache(*context_, &pc);

      // Indexes to the BodyNode objects associated with each mobilizer.
      const BodyNodeIndex shoulder_node =
          shoulder_mobilizer_->get_topology().body_node;
      const BodyNodeIndex elbow_node =
          elbow_mobilizer_->get_topology().body_node;

      // Expected poses of the outboard frames measured in the inboard frame.
      Isometry3d X_SiSo(AngleAxisd(shoulder_angle, Vector3d::UnitZ()));
      Isometry3d X_EiEo(AngleAxisd(elbow_angle, Vector3d::UnitZ()));

      // Verify the values in the position kinematics cache.
      EXPECT_TRUE(pc.get_X_FM(shoulder_node).matrix().isApprox(
          X_SiSo.matrix()));
      EXPECT_TRUE(pc.get_X_FM(elbow_node).matrix().isApprox(
          X_EiEo.matrix()));

      // Verify that both, const and mutable versions point to the same address.
      EXPECT_EQ(&pc.get_X_FM(shoulder_node),
                &pc.get_mutable_X_FM(shoulder_node));
      EXPECT_EQ(&pc.get_X_FM(elbow_node),
                &pc.get_mutable_X_FM(elbow_node));

      // Retrieve body poses from position kinematics cache.
      const Isometry3d& X_WW = get_body_pose_in_world(pc, *world_body_);
      const Isometry3d& X_WU = get_body_pose_in_world(pc, *upper_link_);
      const Isometry3d& X_WL = get_body_pose_in_world(pc, *lower_link_);

      const Isometry3d X_WU_expected =
          acrobot_benchmark_.CalcLink1PoseInWorldFrame(shoulder_angle);

      const Isometry3d X_WL_expected =
          acrobot_benchmark_.CalcElbowOutboardFramePoseInWorldFrame(
              shoulder_angle, elbow_angle);

      // Asserts that the retrieved poses match with the ones specified by the
      // unit test method SetPendulumPoses().
      EXPECT_TRUE(X_WW.matrix().isApprox(Matrix4d::Identity(), kTolerance));
      EXPECT_TRUE(X_WU.matrix().isApprox(X_WU_expected.matrix(), kTolerance));
      EXPECT_TRUE(X_WL.matrix().isApprox(X_WL_expected.matrix(), kTolerance));
    }
  }
}

TEST_F(PendulumKinematicTests, CalcVelocityAndAccelerationKinematics) {
  // This is the minimum factor of the machine precision within which these
  // tests pass. There is an additional factor of two (2) to be on the safe side
  // on other architectures (particularly in Macs).
  const int kEpsilonFactor = 30;
  const double kTolerance = kEpsilonFactor * kEpsilon;

  PositionKinematicsCache<double> pc(model_->get_topology());
  VelocityKinematicsCache<double> vc(model_->get_topology());
  AccelerationKinematicsCache<double> ac(model_->get_topology());

  const int num_angles = 50;
  const double kDeltaAngle = 2 * M_PI / (num_angles - 1.0);
  for (double ishoulder = 0; ishoulder < num_angles; ++ishoulder) {
    const double shoulder_angle = -M_PI + ishoulder * kDeltaAngle;
    for (double ielbow = 0; ielbow < num_angles; ++ielbow) {
      const double elbow_angle = -M_PI + ielbow * kDeltaAngle;

      // ======================================================================
      // Compute position kinematics.
      shoulder_mobilizer_->set_angle(context_.get(), shoulder_angle);
      elbow_mobilizer_->set_angle(context_.get(), elbow_angle);
      model_->CalcPositionKinematicsCache(*context_, &pc);

      // Obtain the lower link center of mass to later shift its computed
      // spatial velocity and acceleration to the center of mass frame for
      // comparison with the benchmark.
      const Isometry3d& X_WL = get_body_pose_in_world(pc, *lower_link_);
      const Matrix3d R_WL = X_WL.linear();
      const Vector3d p_LoLcm_L = lower_link_->get_default_com();
      const Vector3d p_LoLcm_W = R_WL * p_LoLcm_L;

      // ======================================================================
      // Compute velocity kinematics

      // Set the shoulder's angular velocity.
      const double shoulder_angle_rate = 1.0;
      shoulder_mobilizer_->set_angular_rate(context_.get(),
                                            shoulder_angle_rate);
      EXPECT_EQ(shoulder_mobilizer_->get_angular_rate(*context_),
                shoulder_angle_rate);

      // Set the elbow's angular velocity.
      const double elbow_angle_rate = -0.5;
      elbow_mobilizer_->set_angular_rate(context_.get(),
                                         elbow_angle_rate);
      EXPECT_EQ(elbow_mobilizer_->get_angular_rate(*context_),
                elbow_angle_rate);
      model_->CalcVelocityKinematicsCache(*context_, pc, &vc);

      // Retrieve body spatial velocities from velocity kinematics cache.
      const SpatialVelocity<double>& V_WUcm =
          get_body_spatial_velocity_in_world(vc, *upper_link_);
      const SpatialVelocity<double>& V_WL =
          get_body_spatial_velocity_in_world(vc, *lower_link_);
      // Obtain the lower link's center of mass frame spatial velocity by
      // shifting V_WL:
      const SpatialVelocity<double> V_WLcm = V_WL.Shift(p_LoLcm_W);

      const SpatialVelocity<double> V_WUcm_expected(
          acrobot_benchmark_.CalcLink1SpatialVelocityInWorldFrame(
              shoulder_angle, shoulder_angle_rate));
      const SpatialVelocity<double> V_WLcm_expected(
          acrobot_benchmark_.CalcLink2SpatialVelocityInWorldFrame(
              shoulder_angle, elbow_angle,
              shoulder_angle_rate, elbow_angle_rate));

      EXPECT_TRUE(V_WUcm.IsApprox(V_WUcm_expected, kTolerance));
      EXPECT_TRUE(V_WLcm.IsApprox(V_WLcm_expected, kTolerance));

      // ======================================================================
      // Compute acceleration kinematics
      // Test a number of acceleration configurations.
      // For zero vdot:
      VectorX<double> vdot(2);  // Vector of generalized accelerations.
      vdot = VectorX<double>::Zero(2);

      model_->CalcAccelerationKinematicsCache(*context_, pc, vc, vdot, &ac);

      // Retrieve body spatial accelerations from acceleration kinematics cache.
      SpatialAcceleration<double> A_WUcm =
          get_body_spatial_acceleration_in_world(ac, *upper_link_);
      SpatialAcceleration<double> A_WL =
          get_body_spatial_acceleration_in_world(ac, *lower_link_);
      // Obtain the lower link's center of mass frame spatial acceleration by
      // shifting A_WL:
      const Vector3d& w_WL = V_WL.rotational();
      SpatialAcceleration<double> A_WLcm = A_WL.Shift(p_LoLcm_W, w_WL);

      SpatialAcceleration<double> A_WUcm_expected(
          acrobot_benchmark_.CalcLink1SpatialAccelerationInWorldFrame(
              shoulder_angle, shoulder_angle_rate, vdot(0)));

      SpatialAcceleration<double> A_WLcm_expected(
          acrobot_benchmark_.CalcLink2SpatialAccelerationInWorldFrame(
              shoulder_angle, elbow_angle,
              shoulder_angle_rate, elbow_angle_rate,
              vdot(0), vdot(1)));

      EXPECT_TRUE(A_WUcm.IsApprox(A_WUcm_expected, kTolerance));
      EXPECT_TRUE(A_WLcm.IsApprox(A_WLcm_expected, kTolerance));

      // For a non-zero vdot [rad/sec^2]:
      shoulder_mobilizer_->get_mutable_accelerations_from_array(
          &vdot)(0) = -1.0;
      elbow_mobilizer_->get_mutable_accelerations_from_array(&vdot)(0) = 2.0;
      EXPECT_EQ(
          shoulder_mobilizer_->get_accelerations_from_array(vdot).size(), 1);
      EXPECT_EQ(
          shoulder_mobilizer_->get_accelerations_from_array(vdot)(0), -1.0);
      EXPECT_EQ(
          elbow_mobilizer_->get_accelerations_from_array(vdot).size(), 1);
      EXPECT_EQ(
          elbow_mobilizer_->get_accelerations_from_array(vdot)(0), 2.0);

      model_->CalcAccelerationKinematicsCache(*context_, pc, vc, vdot, &ac);

      // Retrieve body spatial accelerations from acceleration kinematics cache.
      A_WUcm = get_body_spatial_acceleration_in_world(ac, *upper_link_);
      A_WL = get_body_spatial_acceleration_in_world(ac, *lower_link_);
      A_WLcm = A_WL.Shift(p_LoLcm_W, w_WL);

      A_WUcm_expected = SpatialAcceleration<double>(
          acrobot_benchmark_.CalcLink1SpatialAccelerationInWorldFrame(
              shoulder_angle, shoulder_angle_rate, vdot(0)));

      A_WLcm_expected = SpatialAcceleration<double>(
          acrobot_benchmark_.CalcLink2SpatialAccelerationInWorldFrame(
              shoulder_angle, elbow_angle,
              shoulder_angle_rate, elbow_angle_rate,
              vdot(0), vdot(1)));

      EXPECT_TRUE(A_WUcm.IsApprox(A_WUcm_expected, kTolerance));
      EXPECT_TRUE(A_WLcm.IsApprox(A_WLcm_expected, kTolerance));
    }
  }
}

// Compute the bias term containing Coriolis and gyroscopic effects for a
// number of different pendulum configurations.
// This is computed using inverse dynamics with vdot = 0.
TEST_F(PendulumKinematicTests, CoriolisTerm) {
  // C(q, v) should be zero when elbow_angle = 0 independent of the shoulder
  // angle.
  VerifyCoriolisTermViaInverseDynamics(0.0, 0.0);
  VerifyCoriolisTermViaInverseDynamics(M_PI / 3.0, 0.0);

  // Attempt a number of non-zero elbow angles.
  VerifyCoriolisTermViaInverseDynamics(0.0, M_PI / 2.0);
  VerifyCoriolisTermViaInverseDynamics(0.0, M_PI / 3.0);
  VerifyCoriolisTermViaInverseDynamics(0.0, M_PI / 4.0);

  // Repeat previous tests but this time with different non-zero values of the
  // shoulder angle. Results should be independent of the shoulder angle for
  // this double pendulum system.
  VerifyCoriolisTermViaInverseDynamics(M_PI / 3.0, M_PI / 2.0);
  VerifyCoriolisTermViaInverseDynamics(M_PI / 3.0, M_PI / 3.0);
  VerifyCoriolisTermViaInverseDynamics(M_PI / 3.0, M_PI / 4.0);
}

// Compute the mass matrix using the inverse dynamics method.
TEST_F(PendulumKinematicTests, MassMatrix) {
  VerifyMassMatrixViaInverseDynamics(0.0, 0.0);
  VerifyMassMatrixViaInverseDynamics(0.0, M_PI / 2.0);
  VerifyMassMatrixViaInverseDynamics(0.0, M_PI / 3.0);
  VerifyMassMatrixViaInverseDynamics(0.0, M_PI / 4.0);

  // For the double pendulum system it turns out that the mass matrix is only a
  // function of the elbow angle, independent of the shoulder angle.
  // Therefore M(q) = H(elbow_angle). We therefore run the same previous tests
  // with different shoulder angles to verify this is true.
  VerifyMassMatrixViaInverseDynamics(M_PI / 3.0, 0.0);
  VerifyMassMatrixViaInverseDynamics(M_PI / 3.0, M_PI / 2.0);
  VerifyMassMatrixViaInverseDynamics(M_PI / 3.0, M_PI / 3.0);
  VerifyMassMatrixViaInverseDynamics(M_PI / 3.0, M_PI / 4.0);
}

// A test to compute generalized forces due to gravity.
TEST_F(PendulumKinematicTests, GravityTerm) {
  // A list of conditions used for testing.
  std::vector<Vector2d> test_matrix;

  test_matrix.push_back({0.0, 0.0});
  test_matrix.push_back({0.0, M_PI / 2.0});
  test_matrix.push_back({0.0, M_PI / 3.0});
  test_matrix.push_back({0.0, M_PI / 4.0});

  test_matrix.push_back({M_PI / 2.0, M_PI / 2.0});
  test_matrix.push_back({M_PI / 2.0, M_PI / 3.0});
  test_matrix.push_back({M_PI / 2.0, M_PI / 4.0});

  test_matrix.push_back({M_PI / 3.0, M_PI / 2.0});
  test_matrix.push_back({M_PI / 3.0, M_PI / 3.0});
  test_matrix.push_back({M_PI / 3.0, M_PI / 4.0});

  test_matrix.push_back({M_PI / 4.0, M_PI / 2.0});
  test_matrix.push_back({M_PI / 4.0, M_PI / 3.0});
  test_matrix.push_back({M_PI / 4.0, M_PI / 4.0});

  for (const Vector2d& q : test_matrix) {
    VerifyGravityTerm(q);
  }
}

// Compute the spatial velocity of each link as measured in the world frame
// using automatic differentiation through
// MultibodyTree::CalcPositionKinematicsCache(). The results are verified
// comparing with the reference solution provided by benchmarks::Acrobot.
TEST_F(PendulumKinematicTests, CalcVelocityKinematicsWithAutoDiffXd) {
  // This is the minimum factor of the machine precision within which these
  // tests pass.
  const int kEpsilonFactor = 20;
  const double kTolerance = kEpsilonFactor * kEpsilon;

  std::unique_ptr<MultibodyTree<AutoDiffXd>> model_autodiff =
      model_->ToAutoDiffXd();

  const RevoluteMobilizer<AutoDiffXd>& shoulder_mobilizer_autodiff =
      model_autodiff->get_variant(*shoulder_mobilizer_);
  const RevoluteMobilizer<AutoDiffXd>& elbow_mobilizer_autodiff =
      model_autodiff->get_variant(*elbow_mobilizer_);

  const RigidBody<AutoDiffXd>& upper_link_autodiff =
      model_autodiff->get_variant(*upper_link_);
  const RigidBody<AutoDiffXd>& lower_link_autodiff =
      model_autodiff->get_variant(*lower_link_);

  std::unique_ptr<Context<AutoDiffXd>> context_autodiff =
      model_autodiff->CreateDefaultContext();

  PositionKinematicsCache<AutoDiffXd> pc(model_autodiff->get_topology());

  const int num_angles = 50;
  const double kDeltaAngle = 2 * M_PI / (num_angles - 1.0);

  const int num_velocities = 2;
  const double w_WU_min = -1.0;
  const double w_WU_max = 1.0;
  const double w_UL_min = -0.5;
  const double w_UL_max = 0.5;

  const double kDelta_w_WU = (w_WU_max - w_WU_min) / (num_velocities - 1.0);
  const double kDelta_w_UL = (w_UL_max - w_UL_min) / (num_velocities - 1.0);

  // Loops over angular velocities.
  for (int iw_shoulder = 0; iw_shoulder < num_velocities; ++iw_shoulder) {
    const double w_WU = w_WU_min + iw_shoulder * kDelta_w_WU;
    for (int iw_elbow = 0; iw_elbow < num_velocities; ++iw_elbow) {
      const double w_UL = w_UL_min + iw_elbow * kDelta_w_UL;

      // Loops over angles.
      for (double iq_shoulder = 0; iq_shoulder < num_angles; ++iq_shoulder) {
        const AutoDiffXd shoulder_angle(
            -M_PI + iq_shoulder * kDeltaAngle, /* angle value */
            Vector1<double>::Constant(w_WU)  /* angular velocity */);
        for (double iq_elbow = 0; iq_elbow < num_angles; ++iq_elbow) {
          const AutoDiffXd elbow_angle(
              -M_PI + iq_elbow * kDeltaAngle,   /* angle value */
              Vector1<double>::Constant(w_UL) /* angular velocity */);

          // Update position kinematics.
          shoulder_mobilizer_autodiff.set_angle(context_autodiff.get(),
                                                shoulder_angle);
          elbow_mobilizer_autodiff.set_angle(context_autodiff.get(),
                                             elbow_angle);
          model_autodiff->CalcPositionKinematicsCache(*context_autodiff, &pc);

          // Retrieve body poses from position kinematics cache.
          const Isometry3<AutoDiffXd>& X_WU =
              get_body_pose_in_world(pc, upper_link_autodiff);
          const Isometry3<AutoDiffXd>& X_WL =
              get_body_pose_in_world(pc, lower_link_autodiff);

          const Isometry3d X_WU_expected =
              acrobot_benchmark_.CalcLink1PoseInWorldFrame(
                  shoulder_angle.value());

          const Isometry3d X_WL_expected =
              acrobot_benchmark_.CalcElbowOutboardFramePoseInWorldFrame(
                  shoulder_angle.value(), elbow_angle.value());

          // Extract the transformations' values.
          Eigen::MatrixXd X_WU_value =
              math::autoDiffToValueMatrix(X_WU.matrix());
          Eigen::MatrixXd X_WL_value =
              math::autoDiffToValueMatrix(X_WL.matrix());

          // Obtain the lower link center of mass to later shift its computed
          // spatial velocity to the center of mass frame for comparison with
          // the benchmark.
          const Matrix3d R_WL = X_WL_value.block<3, 3>(0, 0);
          const Vector3d p_LoLcm_L = lower_link_->get_default_com();
          const Vector3d p_LoLcm_W = R_WL * p_LoLcm_L;

          // Asserts that the retrieved poses match with the ones specified by
          // the unit test method SetPendulumPoses().
          EXPECT_TRUE(X_WU_value.isApprox(X_WU_expected.matrix(), kTolerance));
          EXPECT_TRUE(X_WL_value.isApprox(X_WL_expected.matrix(), kTolerance));

          // Extract the transformations' time derivatives.
          Eigen::MatrixXd X_WU_dot =
              math::autoDiffToGradientMatrix(X_WU.matrix());
          X_WU_dot.resize(4, 4);
          Eigen::MatrixXd X_WL_dot =
              math::autoDiffToGradientMatrix(X_WL.matrix());
          X_WL_dot.resize(4, 4);

          // Convert transformations' time derivatives to spatial velocities.
          SpatialVelocity<double> V_WUcm =
              ComputeSpatialVelocityFromXdot(X_WU_value, X_WU_dot);
          SpatialVelocity<double> V_WL =
              ComputeSpatialVelocityFromXdot(X_WL_value, X_WL_dot);
          // Obtain the lower link's center of mass frame spatial velocity by
          // shifting V_WL:
          const SpatialVelocity<double> V_WLcm = V_WL.Shift(p_LoLcm_W);

          const SpatialVelocity<double> V_WUcm_expected(
              acrobot_benchmark_.CalcLink1SpatialVelocityInWorldFrame(
                  shoulder_angle.value(), w_WU));
          const SpatialVelocity<double> V_WLcm_expected(
              acrobot_benchmark_.CalcLink2SpatialVelocityInWorldFrame(
                  shoulder_angle.value(), elbow_angle.value(), w_WU, w_UL));

          EXPECT_TRUE(V_WUcm.IsApprox(V_WUcm_expected, kTolerance));
          EXPECT_TRUE(V_WLcm.IsApprox(V_WLcm_expected, kTolerance));
        }  // ielbow
      }  // ishoulder
    }  // iw_elbow
  }  // iw_shoulder
}

}  // namespace
}  // namespace multibody
}  // namespace drake
