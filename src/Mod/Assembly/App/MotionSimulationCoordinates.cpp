// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MotionSimulationCoordinates.h"
#include "AssemblyObject.h"

#include <ranges>
#include <stdexcept>

#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTJoint.h>
#include <OndselSolver/AngleZIeqcJeqc.h>
#include <OndselSolver/Constant.h>
#include <OndselSolver/CREATE.h>
#include <OndselSolver/Joint.h>
#include <OndselSolver/SymbolicParser.h>

using namespace Assembly;
using namespace MbD;

namespace
{
// A passive solver coordinate: follow the solver's iterations, rather than
// reconstructing turns from the (wrapped) output placements. No equations added.
class MotionAngleJoint: public MbD::Joint
{
public:
    void connectsItoJ(EndFrmsptr first, EndFrmsptr second) override
    {
        // Motions replace the original marker end frame with a time-dependent
        // frame. Keep independent, passive copies measuring the actual parts.
        Joint::connectsItoJ(first->newCopyEndFrameqc(), second->newCopyEndFrameqc());
    }
    void initializeGlobally() override
    {
        if (!angle) {
            // Match native joints' two-pass initialization: marker derivative
            // data is ready on the next global initialization pass.
            angle = AngleZIeqcJeqc::With(frmI, frmJ);
            angle->owner = this;
            root()->hasChanged = true;
        }
        else {
            angle->initializeGlobally();
        }
    }
    void initializeLocally() override
    {
        if (angle) {
            angle->initializeLocally();
        }
    }
    void postInput() override { angle->postInput(); }
    void prePosIC() override { angle->prePosIC(); }
    void prePosKine() override { angle->prePosKine(); }
    void postPosICIteration() override { angle->postPosICIteration(); }
    void preVelIC() override { angle->preVelIC(); }
    void preAccIC() override { angle->preAccIC(); }
    void simUpdateAll() override { angle->simUpdateAll(); }
    std::shared_ptr<AngleZIeqcJeqc> angle;
};

class MotionAngle: public ASMTJoint
{
public:
    std::shared_ptr<ItemIJ> mbdClassNew() override
    {
        return CREATE<MotionAngleJoint>::With();
    }
    void updateFromMbD() override
    {
        angles.push_back(std::static_pointer_cast<MotionAngleJoint>(mbdObject)->angle->value());
    }
    std::vector<double> angles;
};

}

void Assembly::recordMotionCoordinates(
    const std::shared_ptr<ASMTAssembly>& mbdAssembly, App::DocumentObject* sim,
    const std::vector<App::DocumentObject*>& joints,
    const std::vector<App::DocumentObject*>& motions)
{
    auto* record = dynamic_cast<App::PropertyBool*>(
        sim ? sim->getPropertyByName("RecordMotionCoordinates") : nullptr);
    if (record && record->getValue()) {
        // Retain coordinates inside the disposable frame store, so the existing
        // sweep state scope restores them along with the original simulation.
        const auto solverJoints = *mbdAssembly->joints;
        for (const auto& joint : solverJoints) {
            const auto documentJoint = std::ranges::find_if(joints, [&](const auto* item) {
                return joint->name == item->getFullName();
            });
            if (documentJoint == joints.end()) {
                continue;  // Ground-to-assembly connectors have no joint coordinate.
            }
            auto enabled = [&](const char* property) {
                auto* flag = dynamic_cast<App::PropertyBool*>(
                    (*documentJoint)->getPropertyByName(property));
                return flag && flag->getValue();
            };
            const bool angularDriver = std::ranges::any_of(motions, [&](auto* motion) {
                auto* reference = dynamic_cast<App::PropertyXLinkSub*>(
                    motion->getPropertyByName("Joint"));
                auto* type = dynamic_cast<App::PropertyEnumeration*>(
                    motion->getPropertyByName("MotionType"));
                return reference && reference->getValue() == *documentJoint && type
                    && std::string(type->getValueAsString()) == "Angular";
            });
            if (!angularDriver && !enabled("EnableAngleMin") && !enabled("EnableAngleMax")) {
                continue;
            }
            auto coordinate = CREATE<MotionAngle>::With();
            coordinate->setName(joint->name + "-MotionAngle");
            coordinate->setMarkerI(joint->markerI);
            coordinate->setMarkerJ(joint->markerJ);
            mbdAssembly->addJoint(coordinate);
        }
    }
}

std::vector<double> AssemblyObject::motionSimulationFrameTimes() const
{
    return mbdAssembly ? *mbdAssembly->times : std::vector<double> {};
}

std::vector<double> AssemblyObject::motionSimulationJointState(
    App::DocumentObject* joint, size_t frame, const std::string& formula, double time) const
{
    if (!mbdAssembly || !joint) {
        throw std::runtime_error("Missing simulation joint");
    }
    const std::string name = joint->getFullName();
    const auto& solverJoints = *mbdAssembly->joints;
    const auto found = std::ranges::find_if(solverJoints, [&](const auto& item) {
        return item->name == name;
    });
    if (found == solverJoints.end()) {
        throw std::runtime_error("Joint does not participate in the simulation");
    }
    std::vector<double> values;
    if (!formula.empty()) {
        auto parser = std::make_shared<SymbolicParser>();
        parser->owner = mbdAssembly.get();
        parser->variables->insert({"time", std::make_shared<Constant>(time)});
        parser->parseString(formula);
        values.push_back(parser->stack->top()->getValue());
    }
    for (const auto& item : solverJoints) {
        if (item->name == name + "-MotionAngle") {
            const auto coordinate = std::dynamic_pointer_cast<MotionAngle>(item);
            if (coordinate && frame < coordinate->angles.size()) {
                values.push_back(coordinate->angles[frame]);
            }
        }
    }
    return values;
}

