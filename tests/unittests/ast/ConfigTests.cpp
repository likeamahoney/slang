// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT

#include "Test.h"

#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

using namespace slang::driver;

TEST_CASE("Duplicate modules in different source libraries") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());
    auto tree3 = SyntaxTree::fromText(R"(
module top;
    mod m();
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;

    auto& lib =
        compilation.getRoot().lookupName<InstanceSymbol>("top.m").getDefinition().sourceLibrary;
    CHECK(&lib == lib1.get());
}

TEST_CASE("Driver library default ordering") {
    auto guard = OS::captureOutput();

    Driver driver;
    driver.addStandardArgs();

    auto testDir = findTestDir();
    auto args = fmt::format("testfoo --libmap \"{0}libtest/testlib.map\" \"{0}libtest/top.sv\"",
                            testDir);
    CHECK(driver.parseCommandLine(args));
    CHECK(driver.processOptions());
    CHECK(driver.parseAllSources());

    auto compilation = driver.createCompilation();
    CHECK(driver.reportCompilation(*compilation, false));

    auto& m = compilation->getRoot().lookupName<InstanceSymbol>("top.m");
    CHECK(m.getDefinition().sourceLibrary.name == "lib1");
}

TEST_CASE("Driver library explicit ordering") {
    auto guard = OS::captureOutput();

    Driver driver;
    driver.addStandardArgs();

    auto testDir = findTestDir();
    auto args = fmt::format(
        "testfoo --libmap \"{0}libtest/testlib.map\" \"{0}libtest/top.sv\" -Llib2,lib1", testDir);
    CHECK(driver.parseCommandLine(args));
    CHECK(driver.processOptions());
    CHECK(driver.parseAllSources());

    auto compilation = driver.createCompilation();
    CHECK(driver.reportCompilation(*compilation, false));

    auto& m = compilation->getRoot().lookupName<InstanceSymbol>("top.m");
    CHECK(m.getDefinition().sourceLibrary.name == "lib2");
}

TEST_CASE("Top module in a library") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module top;
endmodule
)");

    CompilationOptions options;
    options.topModules.emplace("lib1.mod");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto topInstances = compilation.getRoot().topInstances;
    CHECK(topInstances.size() == 1);
    CHECK(topInstances[0]->name == "mod");
}

TEST_CASE("Config block top modules") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    localparam int i = 1;
    design frob;
endconfig

module frob;
endmodule

module bar;
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto topInstances = compilation.getRoot().topInstances;
    CHECK(topInstances.size() == 1);
    CHECK(topInstances[0]->name == "frob");
}

TEST_CASE("Config in library, disambiguate with module name") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule

config cfg;
    design mod;
endconfig
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule

module cfg;
endmodule

config cfg;
    design mod;
endconfig
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());
    auto tree3 = SyntaxTree::fromText(R"(
module mod;
endmodule

config cfg;
    design mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("lib2.cfg:config");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;

    auto top = compilation.getRoot().topInstances[0];
    CHECK(top->name == "mod");
    CHECK(top->getDefinition().sourceLibrary.name == "lib2");
}

TEST_CASE("Config that targets library cell") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
config cfg;
    design lib1.mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config block error missing module") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design frob libfoo.bar;
endconfig
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].code == diag::InvalidTopModule);
    CHECK(diags[1].code == diag::InvalidTopModule);
}

TEST_CASE("Config default liblist") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mod;
endmodule

module top;
    mod m1();
endmodule

config cfg;
    design top;
    default liblist lib1;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;

    auto& m1 = compilation.getRoot().lookupName<InstanceSymbol>("top.m1");
    auto& lib = m1.getDefinition().sourceLibrary;
    CHECK(lib.name == "lib1");
}

TEST_CASE("Config cell overrides") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());
    auto tree2 = SyntaxTree::fromText(R"(
module mmm;
endmodule

module nnn;
endmodule

module top;
    mod m1();
    foo f1();
    bar b1();
    nnn n1();
endmodule

config cfg;
    design top;
    cell mod liblist lib1;
    cell foo use mmm;
    cell bar use lib1.mod;
endconfig
)");

    CompilationOptions options;
    options.topModules.emplace("cfg");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config instance overrides") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b.f2 use bar;
endconfig

module foo;
endmodule

module bar;
endmodule

module baz;
    foo f1(), f2();
endmodule

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& inst = compilation.getRoot().lookupName<InstanceSymbol>("top.b.f2");
    CHECK(inst.getDefinition().name == "bar");
}

TEST_CASE("Config instance override errors") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b.f2 use bar;
    instance top.b.f3 use somelib.foo;
    instance top.b.f4 use lib1.mod;
    instance top.b.f5 use lib1.foo;
    instance top.b.f6 liblist lib1;
    instance top.i.p use foo;
endconfig

module foo;
endmodule

module baz;
    foo f1(), f2();
    foo f3(), f4(), f5(), f6();
endmodule

module top;
    baz b();
    I i();
endmodule

program prog;
endprogram

interface I;
    prog p(), q();
endinterface
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 5);
    CHECK(diags[0].code == diag::UnknownModule);
    CHECK(diags[1].code == diag::UnknownLibrary);
    CHECK(diags[2].code == diag::UnknownModule);
    CHECK(diags[3].code == diag::UnknownModule);
    CHECK(diags[4].code == diag::InvalidInstanceForParent);
}

TEST_CASE("Config inherited liblist") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);
    auto lib2 = std::make_unique<SourceLibrary>("lib2", 2);

    auto tree1 = SyntaxTree::fromText(R"(
module mod;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
module baz;
    mod m();
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib2.get());

    auto tree3 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b liblist lib1 lib2;
endconfig

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    compilation.addSyntaxTree(tree3);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config hierarchical config target") {
    auto lib1 = std::make_unique<SourceLibrary>("lib1", 1);

    auto tree1 = SyntaxTree::fromText(R"(
module qq;
endmodule
)",
                                      SyntaxTree::getDefaultSourceManager(), "source", "", {},
                                      lib1.get());

    auto tree2 = SyntaxTree::fromText(R"(
config cfg1;
    design top;
    instance top.b use cfg2;
endconfig

config cfg2;
    design baz;
    instance baz.f1 use mod;
    instance baz.f1 liblist lib1;
endconfig

module mod;
    qq q1();
endmodule

module baz;
    foo f1();
endmodule

module top;
    baz b();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree1);
    compilation.addSyntaxTree(tree2);
    NO_COMPILATION_ERRORS;
}

TEST_CASE("Config instance paths with two roots") {
    auto tree = SyntaxTree::fromText(R"(
config cfg1;
    design foo bar;
    instance foo.a use m1;
    instance bar.a use m2;
endconfig

module m1;
endmodule

module m2;
endmodule

module foo;
    some_mod a();
endmodule

module bar;
    some_mod a();
endmodule
)");
    CompilationOptions options;
    options.topModules.emplace("cfg1");

    Compilation compilation(options);
    compilation.addSyntaxTree(tree);
    NO_COMPILATION_ERRORS;

    auto& root = compilation.getRoot();
    auto& fooA = root.lookupName<InstanceSymbol>("foo.a");
    CHECK(fooA.getDefinition().name == "m1");

    auto& barA = root.lookupName<InstanceSymbol>("bar.a");
    CHECK(barA.getDefinition().name == "m2");
}
