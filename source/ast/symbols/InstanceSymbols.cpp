//------------------------------------------------------------------------------
// InstanceSymbols.cpp
// Contains instance-related symbol definitions
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/ast/symbols/InstanceSymbols.h"

#include "ParameterBuilder.h"

#include "slang/ast/ASTSerializer.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/CompilationDiags.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/ParserDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/TimeTrace.h"

namespace {

using namespace slang;
using namespace slang::ast;
using namespace slang::parsing;
using namespace slang::syntax;

std::pair<std::string_view, SourceLocation> getNameLoc(const HierarchicalInstanceSyntax& syntax) {
    std::string_view name;
    SourceLocation loc;
    if (syntax.decl) {
        name = syntax.decl->name.valueText();
        loc = syntax.decl->name.location();
    }
    else {
        name = "";
        loc = syntax.getFirstToken().location();
    }
    return std::make_pair(name, loc);
}

class InstanceBuilder {
public:
    InstanceBuilder(const ASTContext& context, const DefinitionSymbol& definition,
                    ParameterBuilder& paramBuilder, const HierarchyOverrideNode* parentOverrideNode,
                    std::span<const AttributeInstanceSyntax* const> attributes,
                    const ResolvedConfig* resolvedConfig, const ConfigBlockSymbol* newConfigRoot,
                    bool isFromBind) :
        compilation(context.getCompilation()),
        context(context), definition(definition), paramBuilder(paramBuilder),
        parentOverrideNode(parentOverrideNode), attributes(attributes),
        resolvedConfig(resolvedConfig), newConfigRoot(newConfigRoot), isFromBind(isFromBind) {}

    Symbol* create(const HierarchicalInstanceSyntax& syntax) {
        path.clear();

        if (!syntax.decl) {
            context.addDiag(diag::InstanceNameRequired, syntax.sourceRange());
            return createInstance(syntax, nullptr);
        }

        const HierarchyOverrideNode* overrideNode = nullptr;
        if (parentOverrideNode) {
            if (auto sit = parentOverrideNode->childrenBySyntax.find(syntax);
                sit != parentOverrideNode->childrenBySyntax.end()) {
                overrideNode = &sit->second;
            }
            else if (auto nit = parentOverrideNode->childrenByName.find(
                         syntax.decl->name.valueText());
                     nit != parentOverrideNode->childrenByName.end()) {
                overrideNode = &nit->second;
            }
        }

        auto dims = syntax.decl->dimensions;
        return recurse(syntax, overrideNode, dims.begin(), dims.end());
    }

private:
    using DimIterator = std::span<VariableDimensionSyntax*>::iterator;

    Compilation& compilation;
    const ASTContext& context;
    const DefinitionSymbol& definition;
    SmallVector<int32_t> path;
    ParameterBuilder& paramBuilder;
    const HierarchyOverrideNode* parentOverrideNode;
    std::span<const AttributeInstanceSyntax* const> attributes;
    const ResolvedConfig* resolvedConfig;
    const ConfigBlockSymbol* newConfigRoot;
    bool isFromBind;

    Symbol* createInstance(const HierarchicalInstanceSyntax& syntax,
                           const HierarchyOverrideNode* overrideNode) {
        paramBuilder.setOverrides(overrideNode);
        auto [name, loc] = getNameLoc(syntax);
        auto inst = compilation.emplace<InstanceSymbol>(compilation, name, loc, definition,
                                                        paramBuilder, /* isUninstantiated */ false,
                                                        isFromBind);
        inst->arrayPath = path.copy(compilation);
        inst->setSyntax(syntax);
        inst->setAttributes(*context.scope, attributes);

        if (resolvedConfig) {
            if (newConfigRoot) {
                auto rc = compilation.emplace<ResolvedConfig>(*newConfigRoot, *inst);
                rc->configRule = resolvedConfig->configRule;
                inst->resolvedConfig = rc;
            }
            else {
                inst->resolvedConfig = resolvedConfig;
            }
        }

        return inst;
    }

    Symbol* recurse(const HierarchicalInstanceSyntax& syntax,
                    const HierarchyOverrideNode* overrideNode, DimIterator it, DimIterator end) {
        if (it == end)
            return createInstance(syntax, overrideNode);

        SLANG_ASSERT(syntax.decl);
        auto nameToken = syntax.decl->name;
        auto createEmpty = [&]() {
            return compilation.emplace<InstanceArraySymbol>(compilation, nameToken.valueText(),
                                                            nameToken.location(),
                                                            std::span<const Symbol* const>{},
                                                            ConstantRange());
        };

        auto& dimSyntax = **it;
        ++it;

        // Evaluate the dimensions of the array. If this fails for some reason,
        // make up an empty array so that we don't get further errors when
        // things try to reference this symbol.
        auto dim = context.evalDimension(dimSyntax, /* requireRange */ true, /* isPacked */ false);
        if (!dim.isRange())
            return createEmpty();

        ConstantRange range = dim.range;
        if (range.width() > compilation.getOptions().maxInstanceArray) {
            auto& diag = context.addDiag(diag::MaxInstanceArrayExceeded, dimSyntax.sourceRange());
            diag << definition.getKindString() << compilation.getOptions().maxInstanceArray;
            return createEmpty();
        }

        SmallVector<const Symbol*> elements;
        for (uint32_t i = 0; i < range.width(); i++) {
            const HierarchyOverrideNode* childOverrides = nullptr;
            if (overrideNode) {
                auto nodeIt = overrideNode->childrenBySyntax.find(i);
                if (nodeIt != overrideNode->childrenBySyntax.end())
                    childOverrides = &nodeIt->second;
            }

            path.push_back(range.lower() + int32_t(i));
            auto symbol = recurse(syntax, childOverrides, it, end);
            path.pop_back();

            symbol->name = "";
            elements.push_back(symbol);
        }

        auto result = compilation.emplace<InstanceArraySymbol>(compilation, nameToken.valueText(),
                                                               nameToken.location(),
                                                               elements.copy(compilation), range);
        result->setSyntax(syntax);

        for (auto element : elements)
            result->addMember(*element);

        return result;
    }
};

void createImplicitNets(const HierarchicalInstanceSyntax& instance, const ASTContext& context,
                        const NetType& netType, SmallSet<std::string_view, 8>& implicitNetNames,
                        SmallVectorBase<const Symbol*>& results) {
    // If no default nettype is set, we don't create implicit nets.
    if (netType.isError())
        return;

    for (auto conn : instance.connections) {
        const PropertyExprSyntax* expr = nullptr;
        switch (conn->kind) {
            case SyntaxKind::OrderedPortConnection:
                expr = conn->as<OrderedPortConnectionSyntax>().expr;
                break;
            case SyntaxKind::NamedPortConnection:
                expr = conn->as<NamedPortConnectionSyntax>().expr;
                break;
            default:
                break;
        }

        if (!expr)
            continue;

        SmallVector<const IdentifierNameSyntax*> implicitNets;
        Expression::findPotentiallyImplicitNets(*expr, context, implicitNets);

        auto& comp = context.getCompilation();
        for (auto ins : implicitNets) {
            if (implicitNetNames.emplace(ins->identifier.valueText()).second)
                results.push_back(&NetSymbol::createImplicit(comp, *ins, netType));
        }
    }
}

void getInstanceArrayDimensions(const InstanceArraySymbol& array,
                                SmallVectorBase<ConstantRange>& dimensions) {
    auto scope = array.getParentScope();
    if (scope && scope->asSymbol().kind == SymbolKind::InstanceArray)
        getInstanceArrayDimensions(scope->asSymbol().as<InstanceArraySymbol>(), dimensions);

    dimensions.push_back(array.range);
}

} // namespace

namespace slang::ast {

std::string_view InstanceSymbolBase::getArrayName() const {
    auto scope = getParentScope();
    if (scope && scope->asSymbol().kind == SymbolKind::InstanceArray)
        return scope->asSymbol().as<InstanceArraySymbol>().getArrayName();

    return name;
}

void InstanceSymbolBase::getArrayDimensions(SmallVectorBase<ConstantRange>& dimensions) const {
    auto scope = getParentScope();
    if (scope && scope->asSymbol().kind == SymbolKind::InstanceArray)
        getInstanceArrayDimensions(scope->asSymbol().as<InstanceArraySymbol>(), dimensions);
}

InstanceSymbol::InstanceSymbol(std::string_view name, SourceLocation loc,
                               InstanceBodySymbol& body) :
    InstanceSymbolBase(SymbolKind::Instance, name, loc),
    body(body) {
    body.parentInstance = this;
}

InstanceSymbol::InstanceSymbol(Compilation& compilation, std::string_view name, SourceLocation loc,
                               const DefinitionSymbol& definition, ParameterBuilder& paramBuilder,
                               bool isUninstantiated, bool isFromBind) :
    InstanceSymbol(name, loc,
                   InstanceBodySymbol::fromDefinition(compilation, definition, loc, paramBuilder,
                                                      isUninstantiated, isFromBind)) {
}

InstanceSymbol& InstanceSymbol::createDefault(Compilation& compilation,
                                              const DefinitionSymbol& definition,
                                              const HierarchyOverrideNode* hierarchyOverrideNode,
                                              SourceLocation locationOverride) {
    auto loc = locationOverride ? locationOverride : definition.location;
    return *compilation.emplace<InstanceSymbol>(
        definition.name, loc,
        InstanceBodySymbol::fromDefinition(compilation, definition, loc,
                                           /* isUninstantiated */ false, hierarchyOverrideNode));
}

InstanceSymbol& InstanceSymbol::createVirtual(
    const ASTContext& context, SourceLocation loc, const DefinitionSymbol& definition,
    const ParameterValueAssignmentSyntax* paramAssignments) {

    ParameterBuilder paramBuilder(*context.scope, definition.name, definition.parameters);
    paramBuilder.setInstanceContext(context);
    if (paramAssignments)
        paramBuilder.setAssignments(*paramAssignments);

    auto& comp = context.getCompilation();
    auto& result = *comp.emplace<InstanceSymbol>(comp, definition.name, loc, definition,
                                                 paramBuilder,
                                                 /* isUninstantiated */ false,
                                                 /* isFromBind */ false);

    // Set the parent pointer so that traversing upwards still works to find
    // the instantiation scope. This "virtual" instance never actually gets
    // added to the scope the proper way as a member.
    result.setParent(*context.scope);
    return result;
}

InstanceSymbol& InstanceSymbol::createInvalid(Compilation& compilation,
                                              const DefinitionSymbol& definition) {
    // Give this instance an empty name so that it can't be referenced by name.
    return *compilation.emplace<InstanceSymbol>(
        "", SourceLocation::NoLocation,
        InstanceBodySymbol::fromDefinition(compilation, definition, definition.location,
                                           /* isUninstantiated */ true, nullptr));
}

static const HierarchyOverrideNode* findParentOverrideNode(const Scope& scope) {
    auto& sym = scope.asSymbol();
    if (sym.kind == SymbolKind::InstanceBody)
        return sym.as<InstanceBodySymbol>().hierarchyOverrideNode;

    auto parentScope = sym.getParentScope();
    SLANG_ASSERT(parentScope);

    auto node = findParentOverrideNode(*parentScope);
    if (!node)
        return nullptr;

    if (sym.kind == SymbolKind::GenerateBlock &&
        parentScope->asSymbol().kind == SymbolKind::GenerateBlockArray) {

        auto it = node->childrenBySyntax.find(sym.as<GenerateBlockSymbol>().constructIndex);
        if (it == node->childrenBySyntax.end())
            return nullptr;

        return &it->second;
    }

    auto syntax = sym.getSyntax();
    SLANG_ASSERT(syntax);

    if (auto it = node->childrenBySyntax.find(*syntax); it != node->childrenBySyntax.end())
        return &it->second;

    if (auto it = node->childrenByName.find(sym.name); it != node->childrenByName.end())
        return &it->second;

    return nullptr;
}

static const ConfigBlockSymbol::InstanceOverride* findInstanceOverrideNode(
    const ResolvedConfig& resolvedConfig, const Scope* scope) {
    auto& instOverrides = resolvedConfig.useConfig.instanceOverrides;
    if (instOverrides.empty())
        return nullptr;

    // Start by walking up our parent stack and recording each symbol
    // so we can walk back downward in the correct order when traversing
    // the instanceOverrides tree.
    SmallVector<const Symbol*> parentStack;
    while (true) {
        auto sym = &scope->asSymbol();
        if (sym->kind == SymbolKind::InstanceBody) {
            sym = sym->as<InstanceBodySymbol>().parentInstance;
            SLANG_ASSERT(sym);
        }

        parentStack.push_back(sym);

        scope = sym->getParentScope();
        if (!scope || &resolvedConfig.rootInstance == sym)
            break;
    }

    auto rootSym = parentStack.back();
    auto rootName = rootSym->kind == SymbolKind::Instance
                        ? rootSym->as<InstanceSymbol>().getDefinition().name
                        : rootSym->name;

    auto rootIt = instOverrides.find(rootName);
    if (rootIt == instOverrides.end())
        return nullptr;

    auto node = &rootIt->second;
    for (size_t i = parentStack.size() - 1; i > 0; i--) {
        auto childIt = node->childNodes.find(parentStack[i - 1]->name);
        if (childIt == node->childNodes.end())
            return nullptr;

        node = &childIt->second;
    }

    return node->childNodes.empty() ? nullptr : node;
}

void InstanceSymbol::fromSyntax(Compilation& comp, const HierarchyInstantiationSyntax& syntax,
                                const ASTContext& context, SmallVectorBase<const Symbol*>& results,
                                SmallVectorBase<const Symbol*>& implicitNets, bool isFromBind) {
    auto defName = syntax.type.valueText();
    TimeTraceScope timeScope("createInstances"sv, [&] { return std::string(defName); });

    // Find our parent instance, if there is one.
    bool isUninstantiated = false;
    bool inChecker = false;
    const InstanceBodySymbol* parentInst = nullptr;
    const Scope* currScope = context.scope;
    do {
        auto& sym = currScope->asSymbol();
        if (sym.kind == SymbolKind::InstanceBody) {
            parentInst = &sym.as<InstanceBodySymbol>();
            isUninstantiated |= parentInst->isUninstantiated;
            break;
        }

        if (sym.kind == SymbolKind::CheckerInstanceBody) {
            auto& body = sym.as<CheckerInstanceBodySymbol>();
            inChecker = true;
            isUninstantiated |= body.isUninstantiated;
            currScope = body.parentInstance->getParentScope();
            continue;
        }

        if (sym.kind == SymbolKind::GenerateBlock) {
            isUninstantiated |= sym.as<GenerateBlockSymbol>().isUninstantiated;
        }
        currScope = sym.getParentScope();
    } while (currScope);

    // If this instance is not instantiated then we'll just fill in a placeholder
    // and move on. This is likely inside an untaken generate branch.
    if (isUninstantiated) {
        UninstantiatedDefSymbol::fromSyntax(comp, syntax, context, results, implicitNets);
        return;
    }

    // Unfortunately this instantiation could be for a checker instead of a
    // module/interface/program, so we're forced to do a real name lookup here
    // in the local scope before doing a global definition lookup.
    if (auto sym = Lookup::unqualified(*context.scope, defName, LookupFlags::AllowDeclaredAfter)) {
        if (sym->kind == SymbolKind::Checker) {
            CheckerInstanceSymbol::fromSyntax(sym->as<CheckerSymbol>(), syntax, context, results,
                                              implicitNets, isFromBind);
            return;
        }
    }

    const DefinitionSymbol* owningDefinition = nullptr;
    const HierarchyOverrideNode* parentOverrideNode = nullptr;
    const ResolvedConfig* resolvedConfig = nullptr;
    SmallSet<std::string_view, 8> implicitNetNames;
    auto& netType = context.scope->getDefaultNetType();

    // Creates instance symbols -- if specificInstance is provided then only that
    // instance will be created, otherwise all instances in the original syntax
    // node will be created in one go.
    auto createInstances = [&](const Symbol* def,
                               const HierarchicalInstanceSyntax* specificInstance,
                               const ConfigRule* confRule, const ConfigBlockSymbol* newConfigRoot) {
        if (!def) {
            UninstantiatedDefSymbol::fromSyntax(comp, syntax, specificInstance, context, results,
                                                implicitNets, implicitNetNames, netType);
            return;
        }

        auto addDiag = [&](DiagCode code) -> Diagnostic& {
            if (confRule) {
                SLANG_ASSERT(specificInstance);
                auto& diag = context.addDiag(code, specificInstance->sourceRange());
                diag.addNote(diag::NoteConfigRule, confRule->sourceRange);
                return diag;
            }
            else {
                auto& diag = context.addDiag(code, syntax.type.range());
                if (specificInstance)
                    diag << specificInstance->sourceRange();
                return diag;
            }
        };

        if (def->kind == SymbolKind::Primitive) {
            PrimitiveInstanceSymbol::fromSyntax(def->as<PrimitiveSymbol>(), syntax, context,
                                                results, implicitNets);
            if (!results.empty()) {
                if (!owningDefinition ||
                    owningDefinition->definitionKind != DefinitionKind::Module || inChecker) {
                    addDiag(diag::InvalidPrimInstanceForParent);
                }
                else if (isFromBind) {
                    addDiag(diag::BindTargetPrimitive);
                }
            }
            return;
        }

        auto& definition = def->as<DefinitionSymbol>();
        definition.noteInstantiated();

        if (inChecker) {
            addDiag(diag::InvalidInstanceForParent)
                << definition.getArticleKindString() << "a checker"sv;
        }
        else if (owningDefinition) {
            auto owningKind = owningDefinition->definitionKind;
            if (owningKind == DefinitionKind::Program ||
                (owningKind == DefinitionKind::Interface &&
                 definition.definitionKind == DefinitionKind::Module)) {
                addDiag(diag::InvalidInstanceForParent) << definition.getArticleKindString()
                                                        << owningDefinition->getArticleKindString();
            }
        }

        if (parentInst && parentInst->isFromBind) {
            if (isFromBind) {
                addDiag(diag::BindUnderBind);
                return;
            }

            // If our parent is from a bind statement, pass down the flag
            // so that we prevent further binds below us too.
            isFromBind = true;
        }

        ParameterBuilder paramBuilder(*context.scope, definition.name, definition.parameters);
        if (syntax.parameters)
            paramBuilder.setAssignments(*syntax.parameters);

        auto localConfig = resolvedConfig;
        if (confRule) {
            SLANG_ASSERT(resolvedConfig);
            auto rc = comp.emplace<ResolvedConfig>(*resolvedConfig);
            rc->configRule = confRule;
            if (confRule->liblist)
                rc->liblist = *confRule->liblist;
            localConfig = rc;
        }

        InstanceBuilder builder(context, definition, paramBuilder, parentOverrideNode,
                                syntax.attributes, localConfig, newConfigRoot, isFromBind);

        if (specificInstance) {
            createImplicitNets(*specificInstance, context, netType, implicitNetNames, implicitNets);
            results.push_back(builder.create(*specificInstance));
        }
        else {
            for (auto instanceSyntax : syntax.instances) {
                createImplicitNets(*instanceSyntax, context, netType, implicitNetNames,
                                   implicitNets);
                results.push_back(builder.create(*instanceSyntax));
            }
        }
    };

    if (parentInst) {
        owningDefinition = &parentInst->getDefinition();

        // In the uncommon case that our parent instance has an override
        // node set, we need to go back and make sure we account for any
        // generate blocks that might actually be along the parent path for
        // the new instances we're creating.
        if (parentInst->hierarchyOverrideNode)
            parentOverrideNode = findParentOverrideNode(*context.scope);

        // Check if our parent has a configuration applied. If so, and if
        // that configuration has instance overrides, we need to check if
        // any of them apply to the instances we're about to create.
        if (parentInst->parentInstance) {
            resolvedConfig = parentInst->parentInstance->resolvedConfig;
            if (resolvedConfig) {
                auto overrideNode = findInstanceOverrideNode(*resolvedConfig, context.scope);
                if (overrideNode) {
                    // We need to handle each instance separately, as the config
                    // rules allow the entire definition and parameter values
                    // to be overridden on a per-instance basis.
                    std::optional<const Symbol*> explicitDef;
                    auto& overrideMap = overrideNode->childNodes;
                    for (auto instSyntax : syntax.instances) {
                        auto instName = instSyntax->decl ? instSyntax->decl->name.valueText()
                                                         : ""sv;
                        if (auto ruleIt = overrideMap.find(instName);
                            ruleIt != overrideMap.end() && ruleIt->second.rule) {
                            // We have an override rule, so use it to lookup the def.
                            auto rule = ruleIt->second.rule;
                            auto def = comp.getDefinition(defName, *context.scope, *rule,
                                                          instSyntax->sourceRange(),
                                                          diag::UnknownModule);

                            // If we got back a config block as the new root we need
                            // to resolve that to an actual def based on the top cell
                            // listed in the config.
                            const ConfigBlockSymbol* newRoot = nullptr;
                            if (def && def->kind == SymbolKind::ConfigBlock) {
                                newRoot = &def->as<ConfigBlockSymbol>();
                                if (newRoot->topCells.size() != 1) {
                                    // TODO: error
                                    def = nullptr;
                                }
                                else {
                                    def = comp.getDefinition(*newRoot, newRoot->topCells[0]);
                                }
                            }

                            createInstances(def, instSyntax, rule, newRoot);
                        }
                        else {
                            // No specific config rule, so use the default lookup behavior.
                            if (!explicitDef) {
                                explicitDef = comp.getDefinition(defName, *context.scope,
                                                                 syntax.type.range(),
                                                                 diag::UnknownModule);
                            }
                            createInstances(*explicitDef, instSyntax, nullptr, nullptr);
                        }
                    }
                    return;
                }
            }
        }
    }

    // Simple case: look up the definition and create all instances in one go.
    auto def = comp.getDefinition(defName, *context.scope, syntax.type.range(),
                                  diag::UnknownModule);
    createInstances(def, nullptr, nullptr, nullptr);
}

void InstanceSymbol::fromFixupSyntax(Compilation& comp, const DefinitionSymbol& definition,
                                     const DataDeclarationSyntax& syntax, const ASTContext& context,
                                     SmallVectorBase<const Symbol*>& results) {
    auto missing = [&](TokenKind tk, SourceLocation loc) {
        return Token::createMissing(comp, tk, loc);
    };

    // Fabricate a fake instantiation syntax to let us reuse all of the real logic
    // for this fixup case.
    SmallVector<TokenOrSyntax, 4> instances;
    for (auto decl : syntax.declarators) {
        auto loc = decl->name.location();
        if (!instances.empty())
            instances.push_back(missing(TokenKind::Comma, loc));

        loc = loc + decl->name.rawText().length();
        context.addDiag(diag::InstanceMissingParens, loc) << definition.getKindString();

        auto instName = comp.emplace<InstanceNameSyntax>(decl->name, decl->dimensions);
        auto instance = comp.emplace<HierarchicalInstanceSyntax>(
            instName, missing(TokenKind::OpenParenthesis, loc), std::span<TokenOrSyntax>(),
            missing(TokenKind::CloseParenthesis, loc));

        instances.push_back(instance);
    }

    auto instantiation = comp.emplace<HierarchyInstantiationSyntax>(
        std::span<AttributeInstanceSyntax*>(), syntax.type->getFirstToken(), nullptr,
        instances.copy(comp), syntax.semi);

    SmallVector<const Symbol*> implicitNets;
    fromSyntax(comp, *instantiation, context, results, implicitNets, /* isFromBind */ false);
    SLANG_ASSERT(implicitNets.empty());
}

const DefinitionSymbol& InstanceSymbol::getDefinition() const {
    return body.getDefinition();
}

bool InstanceSymbol::isModule() const {
    return getDefinition().definitionKind == DefinitionKind::Module;
}

bool InstanceSymbol::isInterface() const {
    return getDefinition().definitionKind == DefinitionKind::Interface;
}

bool InstanceSymbol::isTopLevel() const {
    auto parent = getParentScope();
    return parent && parent->asSymbol().kind == SymbolKind::Root && !body.isUninstantiated;
}

const PortConnection* InstanceSymbol::getPortConnection(const PortSymbol& port) const {
    if (!connectionMap)
        resolvePortConnections();

    auto it = connectionMap->find(reinterpret_cast<uintptr_t>(&port));
    if (it == connectionMap->end())
        return nullptr;

    return reinterpret_cast<const PortConnection*>(it->second);
}

const PortConnection* InstanceSymbol::getPortConnection(const MultiPortSymbol& port) const {
    if (!connectionMap)
        resolvePortConnections();

    auto it = connectionMap->find(reinterpret_cast<uintptr_t>(&port));
    if (it == connectionMap->end())
        return nullptr;

    return reinterpret_cast<const PortConnection*>(it->second);
}

const PortConnection* InstanceSymbol::getPortConnection(const InterfacePortSymbol& port) const {
    if (!connectionMap)
        resolvePortConnections();

    auto it = connectionMap->find(reinterpret_cast<uintptr_t>(&port));
    if (it == connectionMap->end())
        return nullptr;

    return reinterpret_cast<const PortConnection*>(it->second);
}

std::span<const PortConnection* const> InstanceSymbol::getPortConnections() const {
    if (!connectionMap)
        resolvePortConnections();
    return connections;
}

void InstanceSymbol::resolvePortConnections() const {
    // Note: the order of operations here is very subtly important.
    // In order to resolve connections, we need to actually know our list of ports.
    // Asking the body for the list of ports requires fully elaborating the instance,
    // especially because of things like non-ansi port declarations which might be
    // deep in the body. That process of elaboration can actually depend back on the
    // port connections because of interface ports.
    // For example:
    //
    //     interface I #(parameter int i) (); endinterface
    //     module M(I iface, input logic [iface.i - 1 : 0] foo);
    //         localparam int j = $bits(foo);
    //     endmodule
    //
    // In order to resolve connections for an instance of M, we elaborate its body,
    // which then requires evaluating $bits(foo) which then depends on the connection
    // provided to `iface`. In the code, this translates to a reetrant call to this
    // function; the first time we call getPortList() on the body will call back in here.
    auto portList = body.getPortList();
    if (connectionMap)
        return;

    auto scope = getParentScope();
    SLANG_ASSERT(scope);

    auto& comp = scope->getCompilation();
    connectionMap = comp.allocPointerMap();

    auto syntax = getSyntax();
    if (!syntax) {
        // If this is a top level module and we have interface ports, the user has
        // the option of allowing it by automatically instantiating interface instances
        // to connect them to.
        if (isTopLevel() && comp.hasFlag(CompilationFlags::AllowTopLevelIfacePorts))
            connectDefaultIfacePorts();
        return;
    }

    SmallVector<const PortConnection*> conns;
    PortConnection::makeConnections(*this, portList,
                                    syntax->as<HierarchicalInstanceSyntax>().connections, conns);

    auto portIt = portList.begin();
    for (auto conn : conns) {
        SLANG_ASSERT(portIt != portList.end());
        connectionMap->emplace(reinterpret_cast<uintptr_t>(*portIt++),
                               reinterpret_cast<uintptr_t>(conn));
    }

    SLANG_ASSERT(portIt == portList.end());
    connections = conns.copy(comp);
}

void InstanceSymbol::connectDefaultIfacePorts() const {
    auto parent = getParentScope();
    SLANG_ASSERT(parent);

    auto& comp = parent->getCompilation();
    ASTContext context(*parent, LookupLocation::max);

    SmallVector<const PortConnection*> conns;
    for (auto port : body.getPortList()) {
        if (port->kind == SymbolKind::InterfacePort) {
            auto& ifacePort = port->as<InterfacePortSymbol>();
            if (ifacePort.interfaceDef) {
                auto& inst = createDefault(comp, *ifacePort.interfaceDef, nullptr, port->location);
                inst.setParent(*parent);

                auto portRange = SourceRange{ifacePort.location,
                                             ifacePort.location + ifacePort.name.length()};
                auto modport = ifacePort.getModport(context, inst, portRange);

                conns.emplace_back(comp.emplace<PortConnection>(ifacePort, &inst, modport));
                connectionMap->emplace(reinterpret_cast<uintptr_t>(port),
                                       reinterpret_cast<uintptr_t>(conns.back()));
            }
        }
    }
    connections = conns.copy(comp);
}

void InstanceSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("body", body);

    serializer.startArray("connections");
    for (auto conn : getPortConnections()) {
        serializer.startObject();
        conn->serializeTo(serializer);
        serializer.endObject();
    }
    serializer.endArray();
}

InstanceBodySymbol::InstanceBodySymbol(Compilation& compilation, const DefinitionSymbol& definition,
                                       const HierarchyOverrideNode* hierarchyOverrideNode,
                                       bool isUninstantiated, bool isFromBind) :
    Symbol(SymbolKind::InstanceBody, definition.name, definition.location),
    Scope(compilation, this), hierarchyOverrideNode(hierarchyOverrideNode),
    isUninstantiated(isUninstantiated), isFromBind(isFromBind), definition(definition) {

    setParent(*definition.getParentScope(), definition.getIndex());
}

InstanceBodySymbol& InstanceBodySymbol::fromDefinition(
    Compilation& compilation, const DefinitionSymbol& definition, SourceLocation instanceLoc,
    bool isUninstantiated, const HierarchyOverrideNode* hierarchyOverrideNode) {

    ParameterBuilder paramBuilder(*definition.getParentScope(), definition.name,
                                  definition.parameters);
    paramBuilder.setForceInvalidValues(isUninstantiated);
    if (hierarchyOverrideNode)
        paramBuilder.setOverrides(hierarchyOverrideNode);

    return fromDefinition(compilation, definition, instanceLoc, paramBuilder, isUninstantiated,
                          /* isFromBind */ false);
}

InstanceBodySymbol& InstanceBodySymbol::fromDefinition(Compilation& comp,
                                                       const DefinitionSymbol& definition,
                                                       SourceLocation instanceLoc,
                                                       ParameterBuilder& paramBuilder,
                                                       bool isUninstantiated, bool isFromBind) {
    auto overrideNode = paramBuilder.getOverrides();
    auto result = comp.emplace<InstanceBodySymbol>(comp, definition, overrideNode, isUninstantiated,
                                                   isFromBind);

    auto& declSyntax = definition.getSyntax()->as<ModuleDeclarationSyntax>();
    result->setSyntax(declSyntax);

    // Package imports from the header always come first.
    for (auto import : declSyntax.header->imports)
        result->addMembers(*import);

    // Add in all parameter ports.
    SmallVector<const ParameterSymbolBase*> params;
    auto paramIt = definition.parameters.begin();
    while (paramIt != definition.parameters.end()) {
        auto& decl = *paramIt;
        if (!decl.isPortParam)
            break;

        auto& param = paramBuilder.createParam(decl, *result, instanceLoc);
        params.push_back(&param);
        paramIt++;
    }

    if (definition.portList)
        result->addMembers(*definition.portList);

    // Finally add members from the body.
    for (auto member : declSyntax.members) {
        // If this is a parameter declaration we will create the symbol manually
        // as we need to apply any overrides.
        if (member->kind != SyntaxKind::ParameterDeclarationStatement) {
            result->addMembers(*member);
        }
        else {
            auto createParam = [&](auto& declarator) {
                SLANG_ASSERT(paramIt != definition.parameters.end());

                auto& decl = *paramIt;
                SLANG_ASSERT(declarator.name.valueText() == decl.name);

                auto& param = paramBuilder.createParam(decl, *result, instanceLoc);
                params.push_back(&param);
                paramIt++;
            };

            auto paramBase = member->as<ParameterDeclarationStatementSyntax>().parameter;
            if (paramBase->kind == SyntaxKind::ParameterDeclaration) {
                for (auto declarator : paramBase->as<ParameterDeclarationSyntax>().declarators)
                    createParam(*declarator);
            }
            else {
                for (auto declarator : paramBase->as<TypeParameterDeclarationSyntax>().declarators)
                    createParam(*declarator);
            }
        }
    }

    // If there are any bind directives targeting this instance,
    // add them to the end of the scope now.
    if (overrideNode) {
        for (auto bindSyntax : overrideNode->binds)
            result->addDeferredMembers(*bindSyntax);
    }

    if (!definition.bindDirectives.empty()) {
        for (auto bindSyntax : definition.bindDirectives)
            result->addDeferredMembers(*bindSyntax);
        comp.noteInstanceWithDefBind(*result);
    }

    result->parameters = params.copy(comp);
    return *result;
}

const Symbol* InstanceBodySymbol::findPort(std::string_view portName) const {
    for (auto port : getPortList()) {
        if (port->name == portName)
            return port;
    }
    return nullptr;
}

bool InstanceBodySymbol::hasSameType(const InstanceBodySymbol& other) const {
    if (&other == this)
        return true;

    if (&definition != &other.definition)
        return false;

    if (parameters.size() != other.parameters.size())
        return false;

    for (auto li = parameters.begin(), ri = other.parameters.begin(); li != parameters.end();
         li++, ri++) {

        auto& lp = (*li)->symbol;
        auto& rp = (*ri)->symbol;
        if (lp.kind != rp.kind)
            return false;

        if (lp.kind == SymbolKind::Parameter) {
            auto& lv = lp.as<ParameterSymbol>().getValue();
            auto& rv = rp.as<ParameterSymbol>().getValue();
            if (lv != rv)
                return false;
        }
        else {
            auto& lt = lp.as<TypeParameterSymbol>().targetType.getType();
            auto& rt = rp.as<TypeParameterSymbol>().targetType.getType();
            if (!lt.isMatching(rt))
                return false;
        }
    }

    return true;
}

void InstanceBodySymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("definition", definition);
}

std::string_view InstanceArraySymbol::getArrayName() const {
    auto scope = getParentScope();
    if (scope && scope->asSymbol().kind == SymbolKind::InstanceArray)
        return scope->asSymbol().as<InstanceArraySymbol>().getArrayName();

    return name;
}

void InstanceArraySymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("range", range.toString());
}

template<typename TSyntax>
static void createUninstantiatedDef(Compilation& compilation, const TSyntax& syntax,
                                    const HierarchicalInstanceSyntax* instanceSyntax,
                                    std::string_view moduleName, const ASTContext& context,
                                    std::span<const Expression* const> params,
                                    SmallVectorBase<const Symbol*>& results,
                                    SmallVectorBase<const Symbol*>& implicitNets,
                                    SmallSet<std::string_view, 8>& implicitNetNames,
                                    const NetType& netType) {
    createImplicitNets(*instanceSyntax, context, netType, implicitNetNames, implicitNets);

    auto [name, loc] = getNameLoc(*instanceSyntax);
    auto sym = compilation.emplace<UninstantiatedDefSymbol>(name, loc, moduleName, params);
    sym->setSyntax(*instanceSyntax);
    sym->setAttributes(*context.scope, syntax.attributes);
    results.push_back(sym);
}

template<typename TSyntax>
static void createUninstantiatedDefs(Compilation& compilation, const TSyntax& syntax,
                                     std::string_view moduleName, const ASTContext& context,
                                     std::span<const Expression* const> params,
                                     SmallVectorBase<const Symbol*>& results,
                                     SmallVectorBase<const Symbol*>& implicitNets) {
    SmallSet<std::string_view, 8> implicitNetNames;
    auto& netType = context.scope->getDefaultNetType();
    for (auto instanceSyntax : syntax.instances) {
        createUninstantiatedDef(compilation, syntax, instanceSyntax, moduleName, context, params,
                                results, implicitNets, implicitNetNames, netType);
    }
}

static std::span<const Expression* const> createUninstantiatedParams(
    const HierarchyInstantiationSyntax& syntax, const ASTContext& context) {

    SmallVector<const Expression*> params;
    if (syntax.parameters) {
        for (auto expr : syntax.parameters->parameters) {
            // Empty expressions are just ignored here.
            if (expr->kind == SyntaxKind::OrderedParamAssignment) {
                params.push_back(
                    &Expression::bind(*expr->as<OrderedParamAssignmentSyntax>().expr, context));
            }
            else if (expr->kind == SyntaxKind::NamedParamAssignment) {
                if (auto ex = expr->as<NamedParamAssignmentSyntax>().expr)
                    params.push_back(&Expression::bind(*ex, context, ASTFlags::AllowDataType));
            }
        }
    }

    return params.copy(context.getCompilation());
}

void UninstantiatedDefSymbol::fromSyntax(Compilation& compilation,
                                         const HierarchyInstantiationSyntax& syntax,
                                         const ASTContext& parentContext,
                                         SmallVectorBase<const Symbol*>& results,
                                         SmallVectorBase<const Symbol*>& implicitNets) {
    ASTContext context = parentContext.resetFlags(ASTFlags::NonProcedural);
    auto params = createUninstantiatedParams(syntax, context);

    createUninstantiatedDefs(compilation, syntax, syntax.type.valueText(), context, params, results,
                             implicitNets);
}

void UninstantiatedDefSymbol::fromSyntax(
    Compilation& compilation, const syntax::HierarchyInstantiationSyntax& syntax,
    const syntax::HierarchicalInstanceSyntax* specificInstance, const ASTContext& parentContext,
    SmallVectorBase<const Symbol*>& results, SmallVectorBase<const Symbol*>& implicitNets,
    SmallSet<std::string_view, 8>& implicitNetNames, const NetType& netType) {

    ASTContext context = parentContext.resetFlags(ASTFlags::NonProcedural);
    auto params = createUninstantiatedParams(syntax, context);

    if (specificInstance) {
        createUninstantiatedDef(compilation, syntax, specificInstance, syntax.type.valueText(),
                                context, params, results, implicitNets, implicitNetNames, netType);
    }
    else {
        for (auto instanceSyntax : syntax.instances) {
            createUninstantiatedDef(compilation, syntax, instanceSyntax, syntax.type.valueText(),
                                    context, params, results, implicitNets, implicitNetNames,
                                    netType);
        }
    }
}

void UninstantiatedDefSymbol::fromSyntax(Compilation& compilation,
                                         const PrimitiveInstantiationSyntax& syntax,
                                         const ASTContext& parentContext,
                                         SmallVectorBase<const Symbol*>& results,
                                         SmallVectorBase<const Symbol*>& implicitNets) {
    ASTContext context = parentContext.resetFlags(ASTFlags::NonProcedural);
    createUninstantiatedDefs(compilation, syntax, syntax.type.valueText(), context, {}, results,
                             implicitNets);
}

void UninstantiatedDefSymbol::fromSyntax(Compilation& compilation,
                                         const CheckerInstantiationSyntax& syntax,
                                         const ASTContext& parentContext,
                                         SmallVectorBase<const Symbol*>& results,
                                         SmallVectorBase<const Symbol*>& implicitNets) {
    ASTContext context = parentContext.resetFlags(ASTFlags::NonProcedural);
    createUninstantiatedDefs(compilation, syntax, syntax.type->getLastToken().valueText(), context,
                             {}, results, implicitNets);

    for (auto sym : results)
        sym->as<UninstantiatedDefSymbol>().mustBeChecker = true;
}

static const AssertionExpr* bindUnknownPortConn(const ASTContext& context,
                                                const PropertyExprSyntax& syntax) {
    // We have to check for a simple reference to an interface instance or port here,
    // since we don't know whether this is an interface port connection or even
    // a normal connection with a virtual interface type.
    const auto flags = ASTFlags::AllowUnboundedLiteral | ASTFlags::StreamingAllowed;
    const SyntaxNode* node = &syntax;
    if (node->kind == SyntaxKind::SimplePropertyExpr) {
        node = node->as<SimplePropertyExprSyntax>().expr;
        if (node->kind == SyntaxKind::SimpleSequenceExpr) {
            auto& simpSeq = node->as<SimpleSequenceExprSyntax>();
            if (!simpSeq.repetition) {
                auto& comp = context.getCompilation();
                const ExpressionSyntax* expr = simpSeq.expr;
                while (expr->kind == SyntaxKind::ParenthesizedExpression)
                    expr = expr->as<ParenthesizedExpressionSyntax>().expression;

                if (NameSyntax::isKind(expr->kind)) {
                    LookupResult result;
                    Lookup::name(expr->as<NameSyntax>(), context, LookupFlags::None, result);
                    if (result.found) {
                        auto symbol = result.found;
                        if (symbol->kind == SymbolKind::Modport ||
                            symbol->kind == SymbolKind::InterfacePort ||
                            symbol->kind == SymbolKind::Instance ||
                            symbol->kind == SymbolKind::InstanceArray ||
                            symbol->kind == SymbolKind::UninstantiatedDef) {
                            auto hre = comp.emplace<ArbitrarySymbolExpression>(
                                *symbol, comp.getVoidType(), syntax.sourceRange());
                            return comp.emplace<SimpleAssertionExpr>(*hre, std::nullopt);
                        }
                    }
                }

                return comp.emplace<SimpleAssertionExpr>(Expression::bind(*expr, context, flags),
                                                         std::nullopt);
            }
        }
    }

    return &AssertionExpr::bind(syntax,
                                context.resetFlags(ASTFlags::AssertionInstanceArgCheck | flags));
}

std::span<const AssertionExpr* const> UninstantiatedDefSymbol::getPortConnections() const {
    if (!ports) {
        auto syntax = getSyntax();
        auto scope = getParentScope();
        SLANG_ASSERT(syntax && scope);

        auto& comp = scope->getCompilation();
        ASTContext context(*scope, LookupLocation::after(*this));

        SmallVector<const AssertionExpr*> results;
        SmallVector<std::string_view, 8> names;
        for (auto port : syntax->as<HierarchicalInstanceSyntax>().connections) {
            if (port->kind == SyntaxKind::OrderedPortConnection) {
                names.push_back(""sv);
                results.push_back(
                    bindUnknownPortConn(context, *port->as<OrderedPortConnectionSyntax>().expr));
            }
            else if (port->kind == SyntaxKind::NamedPortConnection) {
                auto& npc = port->as<NamedPortConnectionSyntax>();
                names.push_back(npc.name.valueText());

                if (auto ex = npc.expr)
                    results.push_back(bindUnknownPortConn(context, *ex));
            }
        }

        ports = results.copy(comp);
        portNames = names.copy(comp);

        for (auto port : *ports) {
            if (port->kind != AssertionExprKind::Simple ||
                port->as<SimpleAssertionExpr>().repetition) {
                mustBeChecker = true;
                break;
            }
        }
    }
    return *ports;
}

std::span<std::string_view const> UninstantiatedDefSymbol::getPortNames() const {
    if (!ports)
        getPortConnections();
    return portNames;
}

bool UninstantiatedDefSymbol::isChecker() const {
    if (!ports)
        getPortConnections();
    return mustBeChecker;
}

void UninstantiatedDefSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("definitionName", definitionName);

    serializer.startArray("parameters");
    for (auto expr : paramExpressions)
        serializer.serialize(*expr);
    serializer.endArray();

    auto conns = getPortConnections();
    auto names = getPortNames();
    SLANG_ASSERT(conns.size() == names.size());

    serializer.startArray("ports");
    for (size_t i = 0; i < conns.size(); i++) {
        serializer.startObject();
        if (!names[i].empty())
            serializer.write("name", names[i]);

        if (mustBeChecker) {
            serializer.write("expr", *conns[i]);
        }
        else {
            serializer.write("expr", conns[i]->as<SimpleAssertionExpr>().expr);
        }

        serializer.endObject();
    }
    serializer.endArray();
}

namespace {

PrimitiveInstanceSymbol* createPrimInst(Compilation& compilation, const Scope& scope,
                                        const PrimitiveSymbol& primitive,
                                        const HierarchicalInstanceSyntax& syntax,
                                        std::span<const AttributeInstanceSyntax* const> attributes,
                                        SmallVectorBase<int32_t>& path) {
    auto [name, loc] = getNameLoc(syntax);
    auto result = compilation.emplace<PrimitiveInstanceSymbol>(name, loc, primitive);
    result->arrayPath = path.copy(compilation);
    result->setSyntax(syntax);
    result->setAttributes(scope, attributes);
    return result;
}

using DimIterator = std::span<VariableDimensionSyntax*>::iterator;

Symbol* recursePrimArray(Compilation& compilation, const PrimitiveSymbol& primitive,
                         const HierarchicalInstanceSyntax& instance, const ASTContext& context,
                         DimIterator it, DimIterator end,
                         std::span<const AttributeInstanceSyntax* const> attributes,
                         SmallVectorBase<int32_t>& path) {
    if (it == end)
        return createPrimInst(compilation, *context.scope, primitive, instance, attributes, path);

    SLANG_ASSERT(instance.decl);
    auto nameToken = instance.decl->name;
    auto createEmpty = [&]() {
        return compilation.emplace<InstanceArraySymbol>(compilation, nameToken.valueText(),
                                                        nameToken.location(),
                                                        std::span<const Symbol* const>{},
                                                        ConstantRange());
    };

    auto& dimSyntax = **it;
    ++it;

    // Evaluate the dimensions of the array. If this fails for some reason,
    // make up an empty array so that we don't get further errors when
    // things try to reference this symbol.
    auto dim = context.evalDimension(dimSyntax, /* requireRange */ true, /* isPacked */ false);
    if (!dim.isRange())
        return createEmpty();

    ConstantRange range = dim.range;
    if (range.width() > compilation.getOptions().maxInstanceArray) {
        auto& diag = context.addDiag(diag::MaxInstanceArrayExceeded, dimSyntax.sourceRange());
        diag << "primitive"sv << compilation.getOptions().maxInstanceArray;
        return createEmpty();
    }

    SmallVector<const Symbol*> elements;
    for (int32_t i = range.lower(); i <= range.upper(); i++) {
        path.push_back(i);
        auto symbol = recursePrimArray(compilation, primitive, instance, context, it, end,
                                       attributes, path);
        path.pop_back();

        symbol->name = "";
        elements.push_back(symbol);
    }

    auto result = compilation.emplace<InstanceArraySymbol>(compilation, nameToken.valueText(),
                                                           nameToken.location(),
                                                           elements.copy(compilation), range);
    for (auto element : elements)
        result->addMember(*element);

    return result;
}

template<typename TSyntax>
void createPrimitives(const PrimitiveSymbol& primitive, const TSyntax& syntax,
                      const ASTContext& context, SmallVectorBase<const Symbol*>& results,
                      SmallVectorBase<const Symbol*>& implicitNets) {
    SmallSet<std::string_view, 8> implicitNetNames;
    SmallVector<int32_t> path;

    auto& comp = context.getCompilation();
    auto& netType = context.scope->getDefaultNetType();

    for (auto instance : syntax.instances) {
        path.clear();
        createImplicitNets(*instance, context, netType, implicitNetNames, implicitNets);

        if (!instance->decl) {
            results.push_back(createPrimInst(comp, *context.scope, primitive, *instance,
                                             syntax.attributes, path));
        }
        else {
            auto dims = instance->decl->dimensions;
            auto symbol = recursePrimArray(comp, primitive, *instance, context, dims.begin(),
                                           dims.end(), syntax.attributes, path);
            results.push_back(symbol);
        }
    }
}

} // namespace

void PrimitiveInstanceSymbol::fromSyntax(const PrimitiveSymbol& primitive,
                                         const HierarchyInstantiationSyntax& syntax,
                                         const ASTContext& context,
                                         SmallVectorBase<const Symbol*>& results,
                                         SmallVectorBase<const Symbol*>& implicitNets) {
    createPrimitives(primitive, syntax, context, results, implicitNets);
}

void PrimitiveInstanceSymbol::fromSyntax(const PrimitiveInstantiationSyntax& syntax,
                                         const ASTContext& context,
                                         SmallVectorBase<const Symbol*>& results,
                                         SmallVectorBase<const Symbol*>& implicitNets) {
    auto& comp = context.getCompilation();
    auto name = syntax.type.valueText();

    auto missing = [&](TokenKind tk, SourceLocation loc) {
        return Token::createMissing(comp, tk, loc);
    };

    if (syntax.type.kind == TokenKind::Identifier) {
        auto def = comp.getDefinition(name, *context.scope, syntax.type.range(),
                                      diag::UnknownPrimitive);
        if (def) {
            if (def->kind == SymbolKind::Primitive) {
                createPrimitives(def->as<PrimitiveSymbol>(), syntax, context, results,
                                 implicitNets);
                return;
            }

            SLANG_ASSERT(syntax.strength || syntax.delay);
            if (syntax.strength) {
                context.addDiag(diag::InstanceWithStrength, syntax.strength->sourceRange()) << name;
            }
            else if (comp.hasFlag(CompilationFlags::AllowBareValParamAssignment) &&
                     syntax.delay->kind == SyntaxKind::DelayControl) {
                // We're allowing this to be a hierarchical instantiation with a single param
                // assignment, and just pretending the parentheses were provided.
                auto& delay = syntax.delay->as<DelaySyntax>();
                auto& delayVal = *delay.delayValue;

                SmallVector<TokenOrSyntax> parameters;
                parameters.push_back(comp.emplace<OrderedParamAssignmentSyntax>(delayVal));

                auto pvas = comp.emplace<ParameterValueAssignmentSyntax>(
                    delay.hash,
                    missing(TokenKind::OpenParenthesis, delayVal.getFirstToken().location()),
                    parameters.copy(comp),
                    missing(TokenKind::CloseParenthesis, delayVal.getLastToken().location()));

                auto instantiation = comp.emplace<HierarchyInstantiationSyntax>(
                    syntax.attributes, syntax.type, pvas, syntax.instances, syntax.semi);
                InstanceSymbol::fromSyntax(comp, *instantiation, context, results, implicitNets,
                                           /* isFromBind */ false);
                return;
            }
            else {
                context.addDiag(diag::InstanceWithDelay,
                                syntax.delay->getFirstToken().location() + 1);
            }
        }
        UninstantiatedDefSymbol::fromSyntax(comp, syntax, context, results, implicitNets);
    }
    else {
        auto prim = comp.getGateType(name);
        SLANG_ASSERT(prim);
        createPrimitives(*prim, syntax, context, results, implicitNets);
    }
}

std::span<const Expression* const> PrimitiveInstanceSymbol::getPortConnections() const {
    if (!ports) {
        auto syntax = getSyntax();
        auto scope = getParentScope();
        SLANG_ASSERT(syntax && scope);

        auto& comp = scope->getCompilation();
        ASTContext context(*scope, LookupLocation::after(*this), ASTFlags::NonProcedural);
        context.setInstance(*this);

        SmallVector<const ExpressionSyntax*> conns;
        auto& his = syntax->as<HierarchicalInstanceSyntax>();
        for (auto port : his.connections) {
            if (port->kind == SyntaxKind::OrderedPortConnection) {
                auto expr = context.requireSimpleExpr(
                    *port->as<OrderedPortConnectionSyntax>().expr);
                if (!expr) {
                    ports.emplace();
                    return *ports;
                }

                conns.push_back(expr);
            }
            else if (port->kind != SyntaxKind::EmptyPortConnection ||
                     primitiveType.primitiveKind != PrimitiveSymbol::UserDefined) {
                context.addDiag(diag::InvalidPrimitivePortConn, port->sourceRange());
                ports.emplace();
                return *ports;
            }
            else {
                context.addDiag(diag::EmptyUdpPort, port->sourceRange());
                conns.push_back(nullptr);
            }
        }

        SmallVector<const Expression*> results;
        if (primitiveType.primitiveKind == PrimitiveSymbol::NInput ||
            primitiveType.primitiveKind == PrimitiveSymbol::NOutput) {
            // Some of the built-in gates allow n-inputs or n-outputs; handle those specially.
            if (conns.size() < 2) {
                auto& diag = context.addDiag(diag::InvalidNGateCount, his.openParen.location());
                diag << primitiveType.name;
                ports.emplace();
                return *ports;
            }

            for (size_t i = 0; i < conns.size(); i++) {
                ArgumentDirection dir;
                if (primitiveType.primitiveKind == PrimitiveSymbol::NInput)
                    dir = i == 0 ? ArgumentDirection::Out : ArgumentDirection::In;
                else
                    dir = conns.size() - 1 ? ArgumentDirection::In : ArgumentDirection::Out;

                SLANG_ASSERT(conns[i]);
                results.push_back(
                    &Expression::bindArgument(comp.getLogicType(), dir, *conns[i], context));
            }
        }
        else {
            if (conns.size() != primitiveType.ports.size()) {
                auto& diag = context.addDiag(diag::PrimitivePortCountWrong,
                                             his.openParen.location());
                diag << primitiveType.name;
                diag << conns.size() << primitiveType.ports.size();
                ports.emplace();
                return *ports;
            }

            for (size_t i = 0; i < conns.size(); i++) {
                if (!conns[i])
                    continue;

                ArgumentDirection dir = ArgumentDirection::In;
                switch (primitiveType.ports[i]->direction) {
                    case PrimitivePortDirection::In:
                        dir = ArgumentDirection::In;
                        break;
                    case PrimitivePortDirection::InOut:
                        dir = ArgumentDirection::InOut;
                        break;
                    case PrimitivePortDirection::Out:
                    case PrimitivePortDirection::OutReg:
                        dir = ArgumentDirection::Out;
                        break;
                }
                results.push_back(
                    &Expression::bindArgument(comp.getLogicType(), dir, *conns[i], context));
            }
        }

        ports = results.copy(scope->getCompilation());
    }
    return *ports;
}

const TimingControl* PrimitiveInstanceSymbol::getDelay() const {
    if (delay)
        return *delay;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    if (!scope || !syntax || !syntax->parent) {
        delay = nullptr;
        return nullptr;
    }

    ASTContext context(*scope, LookupLocation::before(*this), ASTFlags::NonProcedural);

    auto& parent = *syntax->parent;
    if (parent.kind == SyntaxKind::HierarchyInstantiation) {
        if (auto params = parent.as<HierarchyInstantiationSyntax>().parameters) {
            delay = &Delay3Control::fromParams(scope->getCompilation(), *params, context);
            if (delay.value()->kind == TimingControlKind::Delay3) {
                if (auto d3 = delay.value()->as<Delay3Control>().expr3)
                    context.addDiag(diag::Delay3UdpNotAllowed, d3->sourceRange);
            }
            return *delay;
        }
    }
    else {
        auto delaySyntax = parent.as<PrimitiveInstantiationSyntax>().delay;
        if (delaySyntax) {
            delay = &TimingControl::bind(*delaySyntax, context);
            return *delay;
        }
    }

    delay = nullptr;
    return nullptr;
}

std::pair<std::optional<DriveStrength>, std::optional<DriveStrength>> PrimitiveInstanceSymbol::
    getDriveStrength() const {

    auto syntax = getSyntax();
    if (syntax && syntax->parent && syntax->parent->kind == SyntaxKind::PrimitiveInstantiation) {
        auto& pis = syntax->parent->as<PrimitiveInstantiationSyntax>();
        if (pis.strength)
            return SemanticFacts::getDriveStrength(*pis.strength);
    }
    return {};
}

void PrimitiveInstanceSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("primitiveType", primitiveType);

    serializer.startArray("ports");
    for (auto expr : getPortConnections())
        serializer.serialize(*expr);
    serializer.endArray();

    if (auto delayCtrl = getDelay())
        serializer.write("delay", *delayCtrl);

    auto [ds0, ds1] = getDriveStrength();
    if (ds0)
        serializer.write("driveStrength0", toString(*ds0));
    if (ds1)
        serializer.write("driveStrength1", toString(*ds1));
}

namespace {

using DimIterator = std::span<VariableDimensionSyntax*>::iterator;

Symbol* recurseCheckerArray(Compilation& comp, const CheckerSymbol& checker,
                            const HierarchicalInstanceSyntax& instance, const ASTContext& context,
                            DimIterator it, DimIterator end,
                            std::span<const AttributeInstanceSyntax* const> attributes,
                            SmallVectorBase<int32_t>& path, bool isProcedural, bool isFromBind) {
    if (it == end) {
        return &CheckerInstanceSymbol::fromSyntax(comp, context, checker, instance, attributes,
                                                  path, isProcedural, isFromBind);
    }

    SLANG_ASSERT(instance.decl);
    auto nameToken = instance.decl->name;
    auto createEmpty = [&]() {
        return comp.emplace<InstanceArraySymbol>(comp, nameToken.valueText(), nameToken.location(),
                                                 std::span<const Symbol* const>{}, ConstantRange());
    };

    auto& dimSyntax = **it;
    ++it;

    // Evaluate the dimensions of the array. If this fails for some reason,
    // make up an empty array so that we don't get further errors when
    // things try to reference this symbol.
    auto dim = context.evalDimension(dimSyntax, /* requireRange */ true, /* isPacked */ false);
    if (!dim.isRange())
        return createEmpty();

    ConstantRange range = dim.range;
    if (range.width() > comp.getOptions().maxInstanceArray) {
        auto& diag = context.addDiag(diag::MaxInstanceArrayExceeded, dimSyntax.sourceRange());
        diag << "checker"sv << comp.getOptions().maxInstanceArray;
        return createEmpty();
    }

    SmallVector<const Symbol*> elements;
    for (int32_t i = range.lower(); i <= range.upper(); i++) {
        path.push_back(i);
        auto symbol = recurseCheckerArray(comp, checker, instance, context, it, end, attributes,
                                          path, isProcedural, isFromBind);
        path.pop_back();

        symbol->name = "";
        elements.push_back(symbol);
    }

    auto result = comp.emplace<InstanceArraySymbol>(comp, nameToken.valueText(),
                                                    nameToken.location(), elements.copy(comp),
                                                    range);
    for (auto element : elements)
        result->addMember(*element);

    return result;
}

template<typename TSyntax>
void createCheckers(const CheckerSymbol& checker, const TSyntax& syntax, const ASTContext& context,
                    SmallVectorBase<const Symbol*>& results,
                    SmallVectorBase<const Symbol*>& implicitNets, bool isProcedural,
                    bool isFromBind) {
    if (syntax.parameters)
        context.addDiag(diag::CheckerParameterAssign, syntax.parameters->sourceRange());

    SmallSet<std::string_view, 8> implicitNetNames;
    SmallVector<int32_t> path;

    auto& comp = context.getCompilation();
    auto& netType = context.scope->getDefaultNetType();

    for (auto instance : syntax.instances) {
        path.clear();

        if (!isProcedural)
            createImplicitNets(*instance, context, netType, implicitNetNames, implicitNets);

        if (!instance->decl) {
            context.addDiag(diag::InstanceNameRequired, instance->sourceRange());
            results.push_back(&CheckerInstanceSymbol::fromSyntax(comp, context, checker, *instance,
                                                                 syntax.attributes, path,
                                                                 isProcedural, isFromBind));
        }
        else {
            auto dims = instance->decl->dimensions;
            auto symbol = recurseCheckerArray(comp, checker, *instance, context, dims.begin(),
                                              dims.end(), syntax.attributes, path, isProcedural,
                                              isFromBind);
            results.push_back(symbol);
        }
    }
}

} // namespace

CheckerInstanceSymbol::CheckerInstanceSymbol(std::string_view name, SourceLocation loc,
                                             CheckerInstanceBodySymbol& body) :
    InstanceSymbolBase(SymbolKind::CheckerInstance, name, loc),
    body(body) {
    body.parentInstance = this;
}

void CheckerInstanceSymbol::fromSyntax(const CheckerSymbol& checker,
                                       const HierarchyInstantiationSyntax& syntax,
                                       const ASTContext& context,
                                       SmallVectorBase<const Symbol*>& results,
                                       SmallVectorBase<const Symbol*>& implicitNets,
                                       bool isFromBind) {
    createCheckers(checker, syntax, context, results, implicitNets,
                   /* isProcedural */ false, isFromBind);
}

void CheckerInstanceSymbol::fromSyntax(const CheckerInstantiationSyntax& syntax,
                                       const ASTContext& context,
                                       SmallVectorBase<const Symbol*>& results,
                                       SmallVectorBase<const Symbol*>& implicitNets,
                                       bool isFromBind) {
    // If this instance is not instantiated then we'll just fill in a placeholder
    // and move on. This is likely inside an untaken generate branch.
    if (context.scope->isUninstantiated()) {
        UninstantiatedDefSymbol::fromSyntax(context.getCompilation(), syntax, context, results,
                                            implicitNets);
        return;
    }

    LookupResult lookupResult;
    Lookup::name(*syntax.type, context, LookupFlags::AllowDeclaredAfter | LookupFlags::NoSelectors,
                 lookupResult);

    lookupResult.reportDiags(context);
    if (!lookupResult.found)
        return;

    auto symbol = lookupResult.found;
    if (symbol->kind != SymbolKind::Checker) {
        if (symbol->kind == SymbolKind::ClassType) {
            context.addDiag(diag::CheckerClassBadInstantiation, syntax.sourceRange())
                << symbol->name;
        }
        else if (symbol->kind == SymbolKind::Subroutine) {
            context.addDiag(diag::CheckerFuncBadInstantiation, syntax.sourceRange())
                << symbol->name;
        }
        else {
            auto& diag = context.addDiag(diag::NotAChecker, syntax.sourceRange());
            diag << symbol->name << symbol->name;
            diag.addNote(diag::NoteDeclarationHere, symbol->location);
        }
        return;
    }

    // Only procedural if declared via a statement.
    const bool isProcedural = syntax.parent &&
                              syntax.parent->kind == SyntaxKind::CheckerInstanceStatement;

    createCheckers(symbol->as<CheckerSymbol>(), syntax, context, results, implicitNets,
                   isProcedural, isFromBind);
}

static const Symbol* createCheckerFormal(Compilation& comp, const AssertionPortSymbol& port,
                                         CheckerInstanceBodySymbol& instance,
                                         const ExpressionSyntax*& outputInitialSyntax,
                                         const ASTContext& context) {
    // Output ports are special; they aren't involved in the rewriting process,
    // they just act like normal formal ports / arguments.
    if (port.direction == ArgumentDirection::Out) {
        auto arg = comp.emplace<FormalArgumentSymbol>(port.name, port.location, *port.direction,
                                                      VariableLifetime::Static);
        arg->getDeclaredType()->setLink(port.declaredType);

        if (auto portSyntax = port.getSyntax()) {
            arg->setSyntax(*portSyntax);
            arg->setAttributes(instance, portSyntax->as<AssertionItemPortSyntax>().attributes);
        }

        if (port.defaultValueSyntax)
            outputInitialSyntax = context.requireSimpleExpr(*port.defaultValueSyntax);

        instance.addMember(*arg);
        return arg;
    }
    else {
        // Clone all of the formal arguments and add them to the instance so that
        // members in the body can reference them.
        auto& cloned = port.clone(instance);
        instance.addMember(cloned);
        return &cloned;
    }
}

CheckerInstanceSymbol& CheckerInstanceSymbol::fromSyntax(
    Compilation& comp, const ASTContext& parentContext, const CheckerSymbol& checker,
    const HierarchicalInstanceSyntax& syntax,
    std::span<const AttributeInstanceSyntax* const> attributes, SmallVectorBase<int32_t>& path,
    bool isProcedural, bool isFromBind) {

    ASTContext context = parentContext;
    auto parentSym = context.tryFillAssertionDetails();

    auto [name, loc] = getNameLoc(syntax);

    uint32_t depth = 0;
    bool parentIsFromBind = false;
    if (parentSym) {
        if (parentSym->kind == SymbolKind::CheckerInstanceBody) {
            auto& checkerBody = parentSym->as<CheckerInstanceBodySymbol>();
            depth = checkerBody.instanceDepth + 1;
            if (depth > comp.getOptions().maxCheckerInstanceDepth) {
                auto& diag = context.addDiag(diag::MaxInstanceDepthExceeded, loc);
                diag << "checker"sv;
                diag << comp.getOptions().maxCheckerInstanceDepth;
                return createInvalid(checker);
            }

            parentIsFromBind = checkerBody.isFromBind;
        }
        else {
            parentIsFromBind = parentSym->as<InstanceBodySymbol>().isFromBind;
        }
    }

    if (parentIsFromBind) {
        if (isFromBind) {
            context.addDiag(diag::BindUnderBind, syntax.sourceRange());
            return createInvalid(checker);
        }

        // If our parent is from a bind statement, pass down the flag
        // so that we prevent further binds below us too.
        isFromBind = true;
    }

    // It's illegal to instantiate checkers inside fork-join blocks.
    auto parentScope = context.scope;
    while (parentScope->asSymbol().kind == SymbolKind::StatementBlock) {
        auto& block = parentScope->asSymbol().as<StatementBlockSymbol>();
        if (block.blockKind != StatementBlockKind::Sequential) {
            parentScope->addDiag(diag::CheckerInForkJoin, syntax.sourceRange());
            break;
        }

        parentScope = block.getParentScope();
        SLANG_ASSERT(parentScope);
    }

    // It's also illegal to instantiate checkers inside the procedures of other checkers.
    if (parentSym && parentSym->kind == SymbolKind::CheckerInstanceBody && isProcedural)
        context.addDiag(diag::CheckerInCheckerProc, syntax.sourceRange());

    auto assertionDetails = comp.allocAssertionDetails();
    auto body = comp.emplace<CheckerInstanceBodySymbol>(comp, checker, *assertionDetails,
                                                        parentContext, depth, isProcedural,
                                                        isFromBind,
                                                        /* isUninstantiated */ false);

    auto checkerSyntax = checker.getSyntax();
    SLANG_ASSERT(checkerSyntax);
    body->setSyntax(*checkerSyntax);

    assertionDetails->symbol = &checker;
    assertionDetails->instanceLoc = loc;

    // Build port connection map from formals to connection expressions.
    SmallVector<Connection> connections;
    size_t orderedIndex = 0;
    PortConnection::ConnMap connMap(syntax.connections, *context.scope, context.getLocation());
    for (auto port : checker.ports) {
        if (port->name.empty())
            continue;

        const ExpressionSyntax* outputInitialSyntax = nullptr;
        auto actualArg = createCheckerFormal(comp, *port, *body, outputInitialSyntax, context);

        const ASTContext* argCtx = &context;
        const PropertyExprSyntax* expr = nullptr;
        std::optional<ASTContext> defValCtx;
        std::span<const AttributeSymbol* const> attrs;

        auto setDefault = [&](std::optional<DeferredSourceRange> explicitRange) {
            if (!port->defaultValueSyntax || port->direction != ArgumentDirection::In) {
                auto code = explicitRange ? diag::CheckerArgCannotBeEmpty : diag::UnconnectedArg;
                auto& diag = context.addDiag(code, explicitRange ? explicitRange->get()
                                                                 : syntax.sourceRange());
                diag << port->name;
            }
            else {
                expr = port->defaultValueSyntax;
                defValCtx.emplace(checker, LookupLocation::after(*port));
                defValCtx->assertionInstance = assertionDetails;
                defValCtx->flags |= ASTFlags::AssertionDefaultArg;
                argCtx = &defValCtx.value();
            }
        };

        auto createImplicitNamed = [&](DeferredSourceRange range,
                                       bool isWildcard) -> const PropertyExprSyntax* {
            LookupFlags flags = isWildcard ? LookupFlags::DisallowWildcardImport
                                           : LookupFlags::None;
            auto symbol = Lookup::unqualified(*context.scope, port->name, flags);
            if (!symbol) {
                // If this is a wildcard connection, we're allowed to use the port's default value,
                // if it has one.
                if (isWildcard && port->defaultValueSyntax &&
                    port->direction == ArgumentDirection::In) {
                    return port->defaultValueSyntax;
                }

                context.addDiag(diag::ImplicitNamedPortNotFound, range.get()) << port->name;
                return nullptr;
            }

            // Create an expression tree that can stand in for this reference.
            auto nameSyntax = comp.emplace<IdentifierNameSyntax>(
                Token(comp, TokenKind::Identifier, {}, port->name, range.get().start()));
            auto seqSyntax = comp.emplace<SimpleSequenceExprSyntax>(*nameSyntax, nullptr);
            return comp.emplace<SimplePropertyExprSyntax>(*seqSyntax);
        };

        if (connMap.usingOrdered) {
            if (orderedIndex >= connMap.orderedConns.size()) {
                orderedIndex++;
                setDefault({});
            }
            else {
                const PortConnectionSyntax& pc = *connMap.orderedConns[orderedIndex++];
                attrs = AttributeSymbol::fromSyntax(pc.attributes, *context.scope,
                                                    context.getLocation());

                if (pc.kind == SyntaxKind::OrderedPortConnection)
                    expr = pc.as<OrderedPortConnectionSyntax>().expr;
                else
                    setDefault(pc);
            }
        }
        else {
            auto it = connMap.namedConns.find(port->name);
            if (it == connMap.namedConns.end()) {
                if (connMap.hasWildcard)
                    expr = createImplicitNamed(connMap.wildcardRange, true);
                else
                    setDefault({});
            }
            else {
                // We have a named connection; there are two possibilities here:
                // - An explicit connection (with an optional expression)
                // - An implicit connection, where we have to look up the name ourselves
                const NamedPortConnectionSyntax& conn = *it->second.first;
                it->second.second = true;

                attrs = AttributeSymbol::fromSyntax(conn.attributes, *context.scope,
                                                    context.getLocation());
                if (conn.openParen) {
                    // For explicit named port connections, having an empty expression means no
                    // connection, so we never take the default value here.
                    expr = conn.expr;
                    if (!expr) {
                        auto& diag = context.addDiag(diag::CheckerArgCannotBeEmpty,
                                                     conn.sourceRange());
                        diag << port->name;
                    }
                }
                else {
                    expr = createImplicitNamed(conn.name.range(), false);
                }
            }
        }

        assertionDetails->argumentMap.emplace(actualArg, std::make_tuple(expr, *argCtx));
        connections.emplace_back(*body, *actualArg, outputInitialSyntax, attrs);
    }

    if (connMap.usingOrdered) {
        if (orderedIndex < connMap.orderedConns.size()) {
            auto connLoc = connMap.orderedConns[orderedIndex]->getFirstToken().location();
            auto& diag = context.addDiag(diag::TooManyPortConnections, connLoc);
            diag << checker.name;
            diag << connMap.orderedConns.size();
            diag << orderedIndex;
        }
    }
    else {
        for (auto& pair : connMap.namedConns) {
            // We marked all the connections that we used, so anything left over is a connection
            // for a non-existent port.
            if (!pair.second.second) {
                auto& diag = context.addDiag(diag::PortDoesNotExist,
                                             pair.second.first->name.location());
                diag << pair.second.first->name.valueText();
                diag << checker.name;
            }
        }
    }

    // Now add all members.
    for (auto member : checkerSyntax->as<CheckerDeclarationSyntax>().members)
        body->addMembers(*member);

    auto instance = comp.emplace<CheckerInstanceSymbol>(name, loc, *body);
    instance->arrayPath = path.copy(comp);
    instance->setSyntax(syntax);
    instance->setAttributes(*context.scope, attributes);
    instance->connections = connections.copy(comp);
    return *instance;
}

CheckerInstanceSymbol& CheckerInstanceSymbol::createInvalid(const CheckerSymbol& checker) {
    auto scope = checker.getParentScope();
    SLANG_ASSERT(scope);

    auto& comp = scope->getCompilation();
    auto assertionDetails = comp.allocAssertionDetails();
    assertionDetails->symbol = &checker;
    assertionDetails->instanceLoc = checker.location;

    ASTContext context(*scope, LookupLocation::after(checker));
    auto body = comp.emplace<CheckerInstanceBodySymbol>(comp, checker, *assertionDetails, context,
                                                        0u,
                                                        /* isProcedural */ false,
                                                        /* isFromBind */ false,
                                                        /* isUninstantiated */ true);

    auto checkerSyntax = checker.getSyntax();
    SLANG_ASSERT(checkerSyntax);
    body->setSyntax(*checkerSyntax);

    SmallVector<Connection> connections;
    for (auto port : checker.ports) {
        if (port->name.empty())
            continue;

        const ExpressionSyntax* outputInitialSyntax = nullptr;
        auto actualArg = createCheckerFormal(comp, *port, *body, outputInitialSyntax, context);

        assertionDetails->argumentMap.emplace(
            actualArg, std::make_tuple((const PropertyExprSyntax*)nullptr, context));
        connections.emplace_back(*body, *actualArg, outputInitialSyntax,
                                 std::span<const AttributeSymbol* const>{});
    }

    for (auto member : checkerSyntax->as<CheckerDeclarationSyntax>().members)
        body->addMembers(*member);

    auto instance = comp.emplace<CheckerInstanceSymbol>(checker.name, checker.location, *body);
    instance->setSyntax(*checkerSyntax);
    instance->connections = connections.copy(comp);
    return *instance;
}

std::span<const CheckerInstanceSymbol::Connection> CheckerInstanceSymbol::getPortConnections()
    const {
    if (connectionsResolved)
        return connections;

    connectionsResolved = true;

    // We prepopulated some of the connection members but still need
    // to resolve the actual argument value and save it.
    for (auto& conn : connections) {
        conn.getOutputInitialExpr();

        auto argIt = body.assertionDetails.argumentMap.find(&conn.formal);
        SLANG_ASSERT(argIt != body.assertionDetails.argumentMap.end());

        auto& [expr, argCtx] = argIt->second;
        if (!expr)
            continue;

        if (conn.formal.kind == SymbolKind::AssertionPort) {
            AssertionInstanceExpression::ActualArg actualArgValue;
            if (AssertionInstanceExpression::checkAssertionArg(
                    *expr, conn.formal.as<AssertionPortSymbol>(), argCtx, actualArgValue,
                    /* isRecursiveProp */ false)) {
                conn.actual = actualArgValue;
            }
        }
        else if (auto exprSyntax = argCtx.requireSimpleExpr(*expr)) {
            ASTContext context = argCtx;
            if (!body.isProcedural)
                context.flags |= ASTFlags::NonProcedural;

            auto& formal = conn.formal.as<FormalArgumentSymbol>();
            conn.actual = &Expression::bindArgument(formal.getType(), formal.direction, *exprSyntax,
                                                    context);
        }
    }

    return connections;
}

const Expression* CheckerInstanceSymbol::Connection::getOutputInitialExpr() const {
    if (!outputInitialExpr) {
        if (outputInitialSyntax) {
            ASTContext context(parent, LookupLocation::after(formal));
            outputInitialExpr = &Expression::bind(*outputInitialSyntax, context);
        }
        else {
            outputInitialExpr = nullptr;
        }
    }
    return *outputInitialExpr;
}

class CheckerMemberVisitor : public ASTVisitor<CheckerMemberVisitor, true, true> {
public:
    CheckerMemberVisitor(const CheckerInstanceBodySymbol& body) : body(body) {}

    void handle(const ProceduralBlockSymbol& symbol) {
        // Everything is allowed in final blocks, and implicit procedures created
        // for asserions should be ignored.
        if (symbol.procedureKind == ProceduralBlockKind::Final || symbol.isFromAssertion)
            return;

        if (symbol.procedureKind == ProceduralBlockKind::Always) {
            body.addDiag(diag::AlwaysInChecker, symbol.location);
            return;
        }

        SLANG_ASSERT(!currBlock);
        currBlock = &symbol;
        visitDefault(symbol);
        currBlock = nullptr;
    }

    void handle(const VariableSymbol& symbol) {
        inAssignmentRhs = true;
        visitDefault(symbol);
        inAssignmentRhs = false;
    }

    void handle(const AssignmentExpression& expr) {
        // Special checking only applies to assignments to
        // checker variables.
        if (auto sym = expr.left().getSymbolReference()) {
            auto scope = sym->getParentScope();
            while (scope) {
                auto& parentSym = scope->asSymbol();
                if (parentSym.kind == SymbolKind::CheckerInstanceBody) {
                    expr.left().visit(*this);

                    auto prev = std::exchange(inAssignmentRhs, true);
                    expr.right().visit(*this);
                    inAssignmentRhs = prev;
                    return;
                }

                if (parentSym.kind == SymbolKind::InstanceBody)
                    break;

                scope = parentSym.getParentScope();
            }
        }

        visitDefault(expr);
    }

    void handle(const CallExpression& expr) {
        if (inAssignmentRhs && expr.hasOutputArgs())
            body.addDiag(diag::CheckerFuncArg, expr.sourceRange);
    }

    template<std::derived_from<Statement> T>
    void handle(const T& stmt) {
        if (!currBlock)
            return;

        auto notAllowed = [&] {
            auto& diag = body.addDiag(diag::InvalidStmtInChecker, stmt.sourceRange);
            switch (currBlock->procedureKind) {
                case ProceduralBlockKind::Initial:
                    diag << "initial"sv;
                    break;
                case ProceduralBlockKind::AlwaysComb:
                    diag << "always_comb"sv;
                    break;
                case ProceduralBlockKind::AlwaysFF:
                    diag << "always_ff"sv;
                    break;
                case ProceduralBlockKind::AlwaysLatch:
                    diag << "always_latch"sv;
                    break;
                default:
                    SLANG_UNREACHABLE;
            }
        };

        auto checkTimed = [&] {
            auto& timed = stmt.template as<TimedStatement>();
            switch (timed.timing.kind) {
                case TimingControlKind::Invalid:
                case TimingControlKind::SignalEvent:
                case TimingControlKind::EventList:
                case TimingControlKind::ImplicitEvent:
                    return true;
                default:
                    body.addDiag(diag::CheckerTimingControl, timed.sourceRange);
                    return false;
            }
        };

        if (currBlock->procedureKind == ProceduralBlockKind::Initial) {
            switch (stmt.kind) {
                case StatementKind::Empty:
                case StatementKind::List:
                    break;
                case StatementKind::Timed:
                    if (!checkTimed())
                        return;
                    break;
                case StatementKind::Block:
                    if (stmt.template as<BlockStatement>().blockKind !=
                        StatementBlockKind::Sequential) {
                        return notAllowed();
                    }
                    break;
                case StatementKind::ImmediateAssertion:
                case StatementKind::ConcurrentAssertion:
                case StatementKind::ProceduralChecker:
                    return;
                default:
                    return notAllowed();
            }
        }
        else {
            switch (stmt.kind) {
                case StatementKind::Empty:
                case StatementKind::List:
                case StatementKind::Return:
                case StatementKind::Continue:
                case StatementKind::Break:
                case StatementKind::Conditional:
                case StatementKind::Case:
                case StatementKind::ForLoop:
                case StatementKind::RepeatLoop:
                case StatementKind::ForeachLoop:
                case StatementKind::WhileLoop:
                case StatementKind::DoWhileLoop:
                case StatementKind::ForeverLoop:
                    break;
                case StatementKind::Timed:
                    if (!checkTimed())
                        return;
                    break;
                case StatementKind::ExpressionStatement: {
                    auto& expr = stmt.template as<ExpressionStatement>().expr;
                    switch (expr.kind) {
                        case ExpressionKind::Call:
                            break;
                        case ExpressionKind::Assignment:
                            if (!expr.template as<AssignmentExpression>().isNonBlocking() &&
                                currBlock->procedureKind == ProceduralBlockKind::AlwaysFF) {
                                body.addDiag(diag::CheckerBlockingAssign, stmt.sourceRange);
                                return;
                            }
                            break;
                        default:
                            return notAllowed();
                    }
                    break;
                }
                case StatementKind::Block:
                    if (stmt.template as<BlockStatement>().blockKind !=
                        StatementBlockKind::Sequential) {
                        return notAllowed();
                    }
                    break;
                case StatementKind::ImmediateAssertion:
                case StatementKind::ConcurrentAssertion:
                case StatementKind::ProceduralChecker:
                    return;
                default:
                    return notAllowed();
            }
        }

        visitDefault(stmt);
    }

private:
    const CheckerInstanceBodySymbol& body;
    const ProceduralBlockSymbol* currBlock = nullptr;
    bool inAssignmentRhs = false;
};

void CheckerInstanceSymbol::verifyMembers() const {
    CheckerMemberVisitor visitor(body);
    body.visit(visitor);
}

void CheckerInstanceSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("body", body);

    serializer.startArray("connections");
    for (auto& conn : getPortConnections()) {
        serializer.startObject();

        serializer.writeLink("formal", conn.formal);
        std::visit(
            [&](auto&& arg) {
                if (arg)
                    serializer.write("actual", *arg);
            },
            conn.actual);

        if (!conn.attributes.empty()) {
            serializer.startArray("attributes");
            for (auto attr : conn.attributes)
                serializer.serialize(*attr);
            serializer.endArray();
        }

        serializer.endObject();
    }
    serializer.endArray();
}

CheckerInstanceBodySymbol::CheckerInstanceBodySymbol(Compilation& compilation,
                                                     const CheckerSymbol& checker,
                                                     AssertionInstanceDetails& assertionDetails,
                                                     const ASTContext& originalContext,
                                                     uint32_t instanceDepth, bool isProcedural,
                                                     bool isFromBind, bool isUninstantiated) :
    Symbol(SymbolKind::CheckerInstanceBody, checker.name, checker.location),
    Scope(compilation, this), checker(checker), assertionDetails(assertionDetails),
    instanceDepth(instanceDepth), isProcedural(isProcedural), isFromBind(isFromBind),
    isUninstantiated(isUninstantiated), originalContext(originalContext) {

    assertionDetails.prevContext = &this->originalContext;

    auto parent = checker.getParentScope();
    SLANG_ASSERT(parent);
    setParent(*parent, checker.getIndex());
}

void CheckerInstanceBodySymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("checker", checker);
    serializer.write("isProcedural", isProcedural);
}

} // namespace slang::ast
