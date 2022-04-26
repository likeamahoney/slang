//------------------------------------------------------------------------------
//! @file ValueSymbol.h
//! @brief Base class for all value symbols
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include "slang/symbols/SemanticFacts.h"
#include "slang/symbols/Symbol.h"
#include "slang/types/DeclaredType.h"

namespace slang {

class EvalContext;
class ProceduralBlockSymbol;

/// A base class for symbols that represent a value (for example a variable or a parameter).
/// The common functionality is that they all have a type.
class ValueSymbol : public Symbol {
public:
    /// Gets the type of the value.
    const Type& getType() const { return declaredType.getType(); }

    /// Sets the type of the value.
    void setType(const Type& type) { declaredType.setType(type); }

    /// Gets access to the symbol's declared type.
    not_null<const DeclaredType*> getDeclaredType() const { return &declaredType; }
    not_null<DeclaredType*> getDeclaredType() { return &declaredType; }

    /// Sets the symbol's declared type.
    void setDeclaredType(const DataTypeSyntax& newType) { declaredType.setTypeSyntax(newType); }
    void setDeclaredType(const DataTypeSyntax& newType,
                         const SyntaxList<VariableDimensionSyntax>& newDimensions) {
        declaredType.setTypeSyntax(newType);
        declaredType.setDimensionSyntax(newDimensions);
    }

    /// Gets the initializer for this value, if it has one.
    const Expression* getInitializer() const { return declaredType.getInitializer(); }

    /// Sets the initializer for this value.
    void setInitializer(const Expression& expr) { declaredType.setInitializer(expr); }

    /// Sets the expression tree used to initialize this value.
    void setInitializerSyntax(const ExpressionSyntax& syntax, SourceLocation initLocation) {
        declaredType.setInitializerSyntax(syntax, initLocation);
    }

    /// Initializes the value's dimension and initializer syntax from the given declarator.
    void setFromDeclarator(const DeclaratorSyntax& decl);

    static bool isKind(SymbolKind kind);

    class Driver {
    public:
        not_null<const Expression*> longestStaticPrefix;
        const Symbol* containingSymbol;
        DriverKind kind;
        bitmask<AssignFlags> flags;

        Driver(DriverKind kind, const Expression& longestStaticPrefix,
               const Symbol* containingSymbol, bitmask<AssignFlags> flags, SourceRange range);

        const Driver* getNextDriver() const { return next; }
        bool isInputPort() const { return flags.has(AssignFlags::InputPort); }
        bool isUnidirectionalPort() const {
            return flags.has(AssignFlags::InputPort | AssignFlags::OutputPort);
        }
        bool isClockVar() const { return flags.has(AssignFlags::ClockVar); }
        bool isLocalVarFormalArg() const {
            return flags.has(AssignFlags::AssertionLocalVarFormalArg);
        }

        bool isInSingleDriverProcedure() const;
        bool isInFunction() const;
        bool isInInitialBlock() const;

        SourceRange getSourceRange() const;

        bool overlaps(EvalContext& evalContext, const Driver& other) const;

    private:
        friend class ValueSymbol;
        mutable const Driver* next = nullptr;
        mutable SourceRange range;
    };

    void addDriver(DriverKind kind, const Expression& longestStaticPrefix,
                   const Symbol* containingSymbol, bitmask<AssignFlags> flags,
                   SourceRange rangeOverride = {}, EvalContext* customEvalContext = nullptr) const;
    const Driver* getFirstDriver() const { return firstDriver; }

protected:
    ValueSymbol(SymbolKind kind, string_view name, SourceLocation location,
                bitmask<DeclaredTypeFlags> flags = DeclaredTypeFlags::None);

private:
    DeclaredType declaredType;
    mutable const Driver* firstDriver = nullptr;
};

} // namespace slang
