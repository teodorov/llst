#include "patterns/DecodeBytecode.h"

static const uint8_t ABABbytecode[] = {
    33,         // 0000 PushArgument 1
    248,        // 0001 DoSpecial branchIfFalse 8
    8,
    0,
    81,         // 0004 PushConstant 1
    246,        // 0005 DoSpecial branch 9
    9,
    0,
    83,         // 0008 PushConstant 3
    34,         // 0009 PushArgument 2
    248,        // 0010 DoSpecial branchIfFalse 17
    17,
    0,
    85,         // 0013 PushConstant 5
    246,        // 0014 DoSpecial branch 18
    18,
    0,
    87,         // 0017 PushConstant 7
    178         // 0018 SendBinary +
};

INSTANTIATE_TEST_CASE_P(_, P_DecodeBytecode,
    ::testing::Values( std::tr1::make_tuple(std::string("Bytecode for ABAB"), std::string(reinterpret_cast<const char*>(ABABbytecode), sizeof(ABABbytecode))) )
);

void checkSendBinaryArg(st::InstructionNode* inst, int idx)
{
    ASSERT_TRUE(idx == 0 || idx == 1);
    st::ControlNode* arg = inst->getArgument(idx);
    {
        SCOPED_TRACE("Each argument of sendBinary is a phi node");
        ASSERT_EQ( st::ControlNode::ntPhi, arg->getNodeType());
    }
    st::PhiNode* phiArg = arg->cast<st::PhiNode>();

    std::vector<st::PhiNode*> phisToCheck;
    switch (idx)
    {
        case 0: {
            // The first arg is a phi containing phis ><
            ASSERT_EQ(2u, phiArg->getInEdges().size());
            st::ControlNode* phi1 = * phiArg->getInEdges().begin();
            st::ControlNode* phi2 = * ++ phiArg->getInEdges().begin();
            phisToCheck.push_back(phi1->cast<st::PhiNode>());
            phisToCheck.push_back(phi2->cast<st::PhiNode>());
        } break;
        case 1: {
            phisToCheck.push_back(phiArg);
        } break;
    }
    ASSERT_GT(phisToCheck.size(), 0u);

    {
        for(std::size_t phiIdx = 0; phiIdx < phisToCheck.size(); phiIdx++) {
            SCOPED_TRACE("Each edge of arg-phi is a PushConstant");
            st::PhiNode* phi = phisToCheck[phiIdx];
            st::PhiNode::TIncomingList incomingList = phi->getIncomingList();
            for(size_t i = 0; i < incomingList.size(); i++) {
                st::PhiNode::TIncoming incoming = incomingList[i];
                ASSERT_EQ( st::ControlNode::ntInstruction, incoming.node->getNodeType() );
                st::InstructionNode* inst = incoming.node->cast<st::InstructionNode>();
                ASSERT_FALSE(inst == NULL);
                ASSERT_EQ( opcode::pushConstant, inst->getInstruction().getOpcode() );
            }
        }
    }
}

TEST_P(P_DecodeBytecode, ABAB)
{
    class ABABProblem: public st::NodeVisitor
    {
    public:
        bool sendBinaryFound;
        ABABProblem(st::ControlGraph* graph) : st::NodeVisitor(graph), sendBinaryFound(false) {}
        virtual bool visitNode(st::ControlNode& node) {
            if ( st::InstructionNode* inst = node.cast<st::InstructionNode>() )
            {
                if (inst->getInstruction().getOpcode() == opcode::sendBinary)
                {
                    sendBinaryFound = true;

                    EXPECT_EQ(4u, inst->getInEdges().size()); // 2 branches + 2 phis
                    EXPECT_EQ(2u, inst->getArgumentsCount());
                    EXPECT_NE(inst->getArgument(0), inst->getArgument(1));

                    {
                        SCOPED_TRACE("Check first arg");
                        checkSendBinaryArg(inst, 0);
                    }
                    {
                        SCOPED_TRACE("Check second arg");
                        checkSendBinaryArg(inst, 1);
                    }

                    return false;
                }
            }
            return true;
        }
    };
    ABABProblem abab(m_cfg);
    abab.run();
    EXPECT_TRUE(abab.sendBinaryFound);
}
