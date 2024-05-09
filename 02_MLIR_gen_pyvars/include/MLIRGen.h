//
// This file implements a simple IR generation targeting MLIR from a Python AST.
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <vector>

#include "mlir/IR/Block.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LogicalResult.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"

#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include "py_ast.h"

class MLIRGen {
public:
  MLIRGen(mlir::MLIRContext &context, const char *srcName)
      : builder(&context), srcFilename(srcName) {}

  /// Public API: convert the AST for a Python module (source file) to an MLIR
  /// Module operation.
  mlir::LogicalResult mlirGen(mod_ty pyModule) {
    auto srcStringAttr = builder.getStringAttr(srcFilename);
    mlir::Location loc = mlir::FileLineColLoc::get(srcStringAttr, 0, 0);

    module = mlir::ModuleOp::create(loc);
    builder.setInsertionPointToEnd(module.getBody());

    // create function main()
    auto mainFuncType =
        mlir::LLVM::LLVMFunctionType::get(builder.getI32Type(), {});
    auto mainFunc =
        builder.create<mlir::LLVM::LLVMFuncOp>(loc, "main", mainFuncType);
    mlir::Block *entryBlock = mainFunc.addEntryBlock();
    builder.setInsertionPointToEnd(entryBlock);

    switch (pyModule->kind) {
    case Module_kind:
      return mlirGen(pyModule->v.Expression.body);
      break;
    default:
      mlir::emitError(loc, "Not supported pyModule->kind = ")
          << pyModule->kind << "\n";
      return mlir::failure();
    }

    // return 0;
    auto constOp =
        builder.create<mlir::LLVM::ConstantOp>(loc, builder.getI32Type(), 0);
    builder.create<mlir::LLVM::ReturnOp>(loc, constOp->getResult(0));
    return mlir::success();
  }

  mlir::ModuleOp getModule() const { return module; }

private:
  mlir::ModuleOp module;
  mlir::OpBuilder builder;

  /// The symbol table maps a variable name to a value in the current scope.
  /// Entering a function creates a new scope, and the function arguments are
  /// added to the mapping. When the processing of a function is terminated, the
  /// scope is destroyed and the mappings created in this scope are dropped.
  llvm::ScopedHashTable<llvm::StringRef, mlir::Value> symbolTable;

  /// A Python source file name.
  llvm::StringRef srcFilename;

  /// Helper conversion for a Python AST location to an MLIR location.
  template <typename T>
  mlir::Location location(T loc) {
    auto srcStringAttr = builder.getStringAttr(srcFilename);
    return mlir::FileLineColLoc::get(srcStringAttr, loc->lineno,
                                     loc->col_offset);
  }

  mlir::FailureOr<mlir::Value> mlirGen(expr_ty expr) {
    auto loc = location(expr);
    switch (expr->kind) {
    case Name_kind:
      switch (expr->v.Name.ctx) {
      case Store:
        mlir::emitError(loc, "Cannot store variable at the right side\n");
        return mlir::failure();
      case Load:
        return symbolTable.lookup(PyUnicode_AsUTF8(expr->v.Name.id));
      case Del: {
        llvm::StringRef varName = PyUnicode_AsUTF8(expr->v.Name.id);
        if (symbolTable.count(varName)) {
          // insert empty mlir::Value (== nullptr) to mark variable as deleted
          symbolTable.insert(varName, mlir::Value());
          return mlir::success();
        } else {
          mlir::emitError(loc, "Variable is not defined! Cannot delete it!\n");
          return mlir::failure();
        }
      } // Del
      } // switch (expr->v.Name.ctx)
      mlir::emitError(loc, "Not supported expr->v.Name.ctx = ")
          << expr->v.Name.ctx << "\n";
      return mlir::failure(); // Name_kind
    case Constant_kind: {
      llvm::StringRef type = PyUnicode_AsUTF8(expr->v.Constant.kind);
      if (type == "int") {
        mlir::Value value = builder.create<mlir::LLVM::ConstantOp>(
            loc, builder.getI64Type(), 0);
        return value;
      } else if (type == "float") {
        mlir::Value value = builder.create<mlir::LLVM::ConstantOp>(
            loc, builder.getF64Type(), 0.0);
        return value;
      }
      mlir::emitError(loc, "Not support constant type '") << type << "'\n";
      return mlir::failure();
    }
    // TODO: other kinds
    default:
      mlir::emitError(loc, "Not supported expr->kind = ") << expr->kind << "\n";
      return mlir::failure();
    }
  }

  mlir::LogicalResult mlirGen(stmt_ty statement) {
    auto loc = location(statement);
    switch (statement->kind) {
    case Assign_kind: {
      // right side expression
      auto valueOrError = mlirGen(statement->v.Assign.value);
      if (mlir::failed(valueOrError))
        return mlir::failure();
      auto rightValue = valueOrError.value();

      // left side
      expr_ty astTarget = (expr_ty)asdl_seq_GET(statement->v.Assign.targets, 0);
      if (astTarget->kind == Tuple_kind) {
        mlir::emitError(loc, "Tuple is not supported at left side\n");
        return mlir::failure();
      }
      switch (astTarget->kind) {
      case Name_kind: {
        llvm::StringRef varName = PyUnicode_AsUTF8(astTarget->v.Name.id);
        switch (astTarget->v.Name.ctx) {
        case Store:
          symbolTable.insert(varName, rightValue);
          break;
        case Load:
          auto value = symbolTable.lookup(varName);
          break;
        }
        return mlir::success();
      }
      default:
        mlir::emitError(loc, "Not supported astTarget->kind: ")
            << astTarget->kind << "\n";
        return mlir::failure();
      }
      return mlir::success();
    }
    default:
      mlir::emitError(loc, "Not supported statement->kind: ")
          << statement->kind << "\n";
      return mlir::failure();
    }
  }

  mlir::LogicalResult mlirGen(asdl_seq *statements) {
    for (int i = 0; i < asdl_seq_LEN(statements); i++) {
      stmt_ty statement = (stmt_ty)asdl_seq_GET(statements, i);
      return mlirGen(statement);
    }
  }
}; // class MLIRGen
