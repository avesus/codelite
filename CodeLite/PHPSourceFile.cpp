#include "PHPSourceFile.h"
#include <wx/ffile.h>
#include "PHPScannerTokens.h"
#include "PHPEntityNamespace.h"
#include "PHPEntityFunction.h"
#include "PHPEntityVariable.h"
#include <wx/arrstr.h>
#include "PHPEntityClass.h"
#include "PHPDocVisitor.h"

#define NEXT_TOKEN_BREAK_IF_NOT(t, action) \
    {                                      \
        if(!NextToken(token)) break;       \
        if(token.type != t) {              \
            action;                        \
            break;                         \
        }                                  \
    }

PHPSourceFile::PHPSourceFile(const wxString& content)
    : m_text(content)
    , m_parseFunctionBody(false)
    , m_depth(0)
    , m_reachedEOF(false)
{
    m_scanner = ::phpLexerNew(content, kPhpLexerOpt_ReturnComments);
}

PHPSourceFile::PHPSourceFile(const wxFileName& filename)
    : m_filename(filename)
    , m_parseFunctionBody(false)
    , m_depth(0)
    , m_reachedEOF(false)
{
    // Filename is kept in absolute path
    m_filename.MakeAbsolute();

    wxString content;
    wxFFile fp(filename.GetFullPath(), "rb");
    if(fp.IsOpened()) {
        fp.ReadAll(&content, wxConvUTF8);
        fp.Close();
    }
    m_text.swap(content);
    m_scanner = ::phpLexerNew(m_text, kPhpLexerOpt_ReturnComments);
}

PHPSourceFile::~PHPSourceFile()
{
    if(m_scanner) {
        ::phpLexerDestroy(&m_scanner);
    }
}

void PHPSourceFile::Parse(int exitDepth)
{
    int retDepth = exitDepth;
    phpLexerToken token;
    while(NextToken(token)) {
        switch(token.type) {
        case '=':
            m_lookBackTokens.clear();
            break;
        case '{':
            m_lookBackTokens.clear();
            break;
        case '}':
            m_lookBackTokens.clear();
            if(m_depth == retDepth) {
                return;
            }
            break;
        case ';':
            m_lookBackTokens.clear();
            break;
        case kPHP_T_PUBLIC:
        case kPHP_T_PRIVATE:
        case kPHP_T_PROTECTED: {
            int visibility = token.type;
            PHPEntityClass* cls = CurrentScope()->Cast<PHPEntityClass>();
            if(cls) {
                m_lookBackTokens.clear();
                /// keep the current token
                m_lookBackTokens.push_back(token);

                // Now we have a small problem here:
                // public can be a start for a member or a function
                // we let the lexer run forward until it finds kPHP_T_VARIABLE (for variable)
                // or kPHP_T_IDENTIFIER
                int what = ReadUntilFoundOneOf(kPHP_T_VARIABLE, kPHP_T_FUNCTION, token);
                if(what == kPHP_T_VARIABLE) {
                    // A variable
                    PHPEntityBase::Ptr_t member(new PHPEntityVariable());
                    member->SetFilename(m_filename.GetFullPath());
                    PHPEntityVariable* var = member->Cast<PHPEntityVariable>();
                    var->SetVisibility(visibility);
                    var->SetName(token.text);
                    var->SetFlag(PHPEntityVariable::kMember);
                    var->SetLine(token.lineNumber);
                    CurrentScope()->AddChild(member);
                    if(!ConsumeUntil(';')) return;
                } else if(what == kPHP_T_FUNCTION) {
                    // A function...
                    OnFunction();
                    m_lookBackTokens.clear();
                }
            }
            break;
        }
        case kPHP_T_CONST:
            if(ReadUntilFound(kPHP_T_IDENTIFIER, token)) {
                // constant
                PHPEntityBase::Ptr_t member(new PHPEntityVariable());
                member->SetFilename(m_filename.GetFullPath());
                PHPEntityVariable* var = member->Cast<PHPEntityVariable>();
                var->SetName(token.text);
                var->SetLine(token.lineNumber);
                var->SetFlag(PHPEntityVariable::kMember);
                CurrentScope()->AddChild(member);
                if(!ConsumeUntil(';')) return;
            }
            break;
        case kPHP_T_REQUIRE:
        case kPHP_T_REQUIRE_ONCE:
        case kPHP_T_INCLUDE:
        case kPHP_T_INCLUDE_ONCE:
            // Handle include files
            m_lookBackTokens.clear();
            break;
        case kPHP_T_USE:
            // Found outer 'use' statement - construct the alias table
            OnUse();
            m_lookBackTokens.clear();
            break;
        case kPHP_T_CLASS:
            // Found class
            OnClass();
            m_lookBackTokens.clear();
            break;
        case kPHP_T_NAMESPACE:
            // Found a namespace
            OnNamespace();
            m_lookBackTokens.clear();
            break;
        case kPHP_T_FUNCTION:
            // Found function
            OnFunction();
            m_lookBackTokens.clear();
            break;
        default:
            // Keep the token
            break;
        }
    }
    PhaseTwo();
}

void PHPSourceFile::OnUse()
{
    wxString fullname, alias, temp;
    phpLexerToken token;
    bool cont = true;
    while(cont && NextToken(token)) {
        switch(token.type) {
        case ',':
        case ';': {
            if(fullname.IsEmpty()) {
                // no full name yet
                fullname.swap(temp);

            } else if(alias.IsEmpty()) {
                alias.swap(temp);
            }

            if(alias.IsEmpty()) {
                // no alias provided, use the last part of the fullname
                alias = fullname.AfterLast('\\');
            }

            if(!fullname.IsEmpty() && !alias.IsEmpty()) {
                m_aliases.insert(std::make_pair(alias, MakeIdentifierAbsolute(fullname)));
            }
            temp.clear();
            fullname.clear();
            alias.clear();
            if(token.type == ';') {
                cont = false;
            }
        } break;
        case kPHP_T_AS: {
            fullname.swap(temp);
            temp.clear();
        } break;
        default:
            temp << token.text;
            break;
        }
    }
}

void PHPSourceFile::OnNamespace()
{
    // Read until we find the line delimiter ';' or EOF found
    wxString path;
    phpLexerToken token;
    while(NextToken(token)) {
        if(token.type == ';') {
            break;
        }

        // Make sure that the namespace path is alway set in absolute path
        // i.e. starts with kPHP_T_NS_SEPARATOR
        if(path.IsEmpty() && token.type != kPHP_T_NS_SEPARATOR) {
            path << "\\";
        }
        path << token.text;
    }

    if(m_scopes.empty()) {
        // no scope is set, push the global scope
        m_scopes.push_back(PHPEntityBase::Ptr_t(new PHPEntityNamespace()));
        PHPEntityNamespace* ns = CurrentScope()->Cast<PHPEntityNamespace>();
        if(ns) {
            ns->SetName(path); // Global namespace
        }
    } else {
        // PHP parsing error... (namespace must be the first thing on the file)
    }
}

void PHPSourceFile::OnFunction()
{
    // read the next token
    phpLexerToken token;
    if(!NextToken(token)) {
        return;
    }

    PHPEntityFunction* func(NULL);
    int funcDepth(0);
    if(token.type == kPHP_T_IDENTIFIER) {
        // the function name
        func = new PHPEntityFunction();
        func->SetName(token.text);
        func->SetLine(token.lineNumber);

    } else if(token.type == '(') {
        funcDepth = 1; // Since we already consumed the open brace
        // anonymous function
        func = new PHPEntityFunction();
        func->SetLine(token.lineNumber);
    }

    if(!func) return;
    PHPEntityBase::Ptr_t funcPtr(func);

    // add the function to the current scope
    CurrentScope()->AddChild(funcPtr);

    // Set the function as the current scope
    m_scopes.push_back(funcPtr);

    // update function attributes
    ParseFunctionSignature(funcDepth);
    func->SetFlags(LookBackForFunctionFlags());

    if(ReadUntilFound('{', token)) {
        // found the function body starting point
        if(IsParseFunctionBody()) {
            ParseFunctionBody();
        } else {
            // Consume the function body
            ConsumeFunctionBody();
        }
    }
    // Remove the current function from the scope list
    if(!m_reachedEOF) {
        m_scopes.pop_back();
    }
    m_lookBackTokens.clear();
}

PHPEntityBase::Ptr_t PHPSourceFile::CurrentScope()
{
    if(m_scopes.empty()) {
        // no scope is set, push the global scope
        m_scopes.push_back(PHPEntityBase::Ptr_t(new PHPEntityNamespace()));
        CurrentScope()->SetName("\\"); // Global namespace
    }
    return m_scopes.back();
}

size_t PHPSourceFile::LookBackForFunctionFlags()
{
    size_t flags(0);
    for(size_t i = 0; i < m_lookBackTokens.size(); ++i) {
        const phpLexerToken& tok = m_lookBackTokens.at(i);
        if(tok.type == kPHP_T_ABSTRACT) {
            flags |= PHPEntityFunction::kAbstract;

        } else if(tok.type == kPHP_T_FINAL) {
            flags |= PHPEntityFunction::kFinal;

        } else if(tok.type == kPHP_T_STATIC) {
            flags |= PHPEntityFunction::kStatic;

        } else if(tok.type == kPHP_T_PUBLIC) {
            flags |= PHPEntityFunction::kPublic;

        } else if(tok.type == kPHP_T_PRIVATE) {
            flags |= PHPEntityFunction::kPrivate;

        } else if(tok.type == kPHP_T_PROTECTED) {
            flags |= PHPEntityFunction::kProtected;
        }
    }
    return flags;
}

void PHPSourceFile::ParseFunctionSignature(int startingDepth)
{
    phpLexerToken token;
    if(startingDepth == 0) {
        // loop until we find the open brace
        while(NextToken(token)) {
            if(token.type == '(') {
                ++startingDepth;
                break;
            }
        }
        if(startingDepth == 0) return;
    }

    // at this point the 'depth' is 1
    int depth = 1;
    wxString typeHint;
    wxString defaultValue;
    PHPEntityVariable* var(NULL);
    bool collectingDefaultValue = false;
    while(NextToken(token)) {
        switch(token.type) {
        case kPHP_T_VARIABLE:
            var = new PHPEntityVariable();
            var->SetName(token.text);
            var->SetLine(token.lineNumber);
            var->SetFilename(m_filename);
            // Mark this variable as function argument
            var->SetFlag(PHPEntityVariable::kFunctionArg);
            if(typeHint.EndsWith("&")) {
                var->SetIsReference(true);
                typeHint.RemoveLast();
            }
            var->SetTypeHint(MakeIdentifierAbsolute(typeHint));
            break;
        case '(':
            depth++;
            if(collectingDefaultValue) {
                defaultValue << "(";
            }
            break;
        case ')':
            depth--;
            // if the depth goes under 1 - we are done
            if(depth < 1) {
                if(var) {
                    var->SetDefaultValue(defaultValue);
                    CurrentScope()->AddChild(PHPEntityBase::Ptr_t(var));
                }
                return;

            } else if(depth) {
                defaultValue << token.text;
            }
            break;
        case '=':
            // default value
            collectingDefaultValue = true;
            break;
        case ',':
            if(var) {
                var->SetDefaultValue(defaultValue);
                CurrentScope()->AddChild(PHPEntityBase::Ptr_t(var));
            }
            var = NULL;
            typeHint.Clear();
            defaultValue.Clear();
            collectingDefaultValue = false;
            break;
        default:
            if(collectingDefaultValue) {
                defaultValue << token.text;
            } else {
                typeHint << token.text;
            }
            break;
        }
    }
}

void PHPSourceFile::PrintStdout()
{
    // print the alias table
    wxPrintf("Alias table:\n");
    wxPrintf("===========\n");
    std::map<wxString, wxString>::iterator iter = m_aliases.begin();
    for(; iter != m_aliases.end(); ++iter) {
        wxPrintf("%s => %s\n", iter->first, iter->second);
    }
    wxPrintf("===========\n");
    if(m_scopes.empty()) return;
    m_scopes.front()->PrintStdout(0);
}

bool PHPSourceFile::ReadUntilFound(int delim, phpLexerToken& token)
{
    // loop until we find the open brace
    while(NextToken(token)) {
        if(token.type == delim) {
            return true;
        }
    }
    return false;
}

void PHPSourceFile::ConsumeFunctionBody()
{
    int depth = m_depth;
    phpLexerToken token;
    while(NextToken(token)) {
        switch(token.type) {
        case '}':
            if(m_depth < depth) {
                return;
            }
            break;
        default:
            break;
        }
    }
}

void PHPSourceFile::ParseFunctionBody()
{
    m_lookBackTokens.clear();

    // when we reach the current depth-1 -> leave
    int exitDepth = m_depth - 1;
    phpLexerToken token;
    PHPEntityBase::Ptr_t var(NULL);
    while(NextToken(token)) {
        switch(token.type) {
        case '{':
            m_lookBackTokens.clear();
            break;
        case '}':
            m_lookBackTokens.clear();
            if(m_depth == exitDepth) {
                return;
            }
            break;
        case ';':
            m_lookBackTokens.clear();
            break;
        case kPHP_T_VARIABLE: {
            var.reset(new PHPEntityVariable());
            var->Cast<PHPEntityVariable>()->SetName(token.text);
            var->Cast<PHPEntityVariable>()->SetFilename(m_filename.GetFullPath());
            var->SetLine(token.lineNumber);
            CurrentScope()->AddChild(var);

            // Peek at the next token
            if(!NextToken(token)) return; // EOF
            if(token.type != '=') {
                m_lookBackTokens.clear();
                var.reset(NULL);
                UngetToken(token);

            } else {

                wxString expr;
                if(!ReadExpression(expr)) return; // EOF

                // Optimize 'new ClassName(..)' expression
                if(expr.StartsWith("new")) {
                    expr = expr.Mid(3);
                    expr.Trim().Trim(false);
                    expr = expr.BeforeFirst('(');
                    expr.Trim().Trim(false);
                    var->Cast<PHPEntityVariable>()->SetTypeHint(MakeIdentifierAbsolute(expr));

                } else {
                    // keep the expression
                    var->Cast<PHPEntityVariable>()->SetExpressionHint(expr);
                }
            }
        } break;
        default:
            break;
        }
    }
}

wxString PHPSourceFile::ReadType()
{
    bool cont = true;
    wxString type;
    phpLexerToken token;
    while(cont && NextToken(token)) {
        switch(token.type) {
        case kPHP_T_IDENTIFIER:
            type << token.text;
            break;

        case kPHP_T_NS_SEPARATOR:
            type << token.text;
            break;

        // special cases that must always be handled
        case '{':
            cont = false;
            break;
        // end of special cases
        default:
            cont = false;
            break;
        }
    }
    type = MakeIdentifierAbsolute(type);
    return type;
}

PHPEntityBase::Ptr_t PHPSourceFile::Namespace()
{
    if(m_scopes.empty()) {
        return CurrentScope();
    }
    return *m_scopes.begin();
}

wxString PHPSourceFile::LookBackForTypeHint()
{
    if(m_lookBackTokens.empty()) return wxEmptyString;
    wxArrayString tokens;

    for(size_t i = m_lookBackTokens.size() - 1; i >= 0; --i) {
        if(m_lookBackTokens.at(i).type == kPHP_T_IDENTIFIER || m_lookBackTokens.at(i).type == kPHP_T_NS_SEPARATOR) {
            tokens.Insert(m_lookBackTokens.at(i).text, 0);
        } else {
            break;
        }
    }

    wxString type;
    for(size_t i = 0; i < tokens.GetCount(); ++i) {
        type << tokens.Item(i);
    }
    return type;
}

void PHPSourceFile::PhaseTwo()
{
    // Assigna doc comment to each of the entities found in this source file
    PHPDocVisitor visitor(*this, m_comments);
    visitor.Visit(Namespace());
}

bool PHPSourceFile::NextToken(phpLexerToken& token)
{
    bool res = ::phpLexerNext(m_scanner, token);
    if(res && token.type == kPHP_T_C_COMMENT) {
        m_comments.push_back(token);
    } else if(token.type == '{') {
        m_depth++;
    } else if(token.type == '}') {
        m_depth--;
    }
    if(!res) m_reachedEOF = true;
    if(res) m_lookBackTokens.push_back(token);
    return res;
}

wxString PHPSourceFile::MakeIdentifierAbsolute(const wxString& type)
{
    wxString typeWithNS(type);
    typeWithNS.Trim().Trim(false);

    if(typeWithNS == "string" || typeWithNS == "array" || typeWithNS == "mixed" || typeWithNS == "bool" ||
       typeWithNS == "int" || typeWithNS == "integer" || typeWithNS == "boolean") {
        // primitives, don't bother...
        return typeWithNS;
    }

    if(typeWithNS.IsEmpty()) return "";

    if(typeWithNS.StartsWith("\\")) {
        return typeWithNS;
    }

    // Use the alias table first
    if(m_aliases.find(type) != m_aliases.end()) {
        return m_aliases.find(type)->second;
    }
    wxString ns = Namespace()->GetName();
    if(!ns.EndsWith("\\")) {
        ns << "\\";
    }

    typeWithNS.Prepend(ns);
    return typeWithNS;
}

void PHPSourceFile::OnClass()
{
    // A "complex" example: class A extends BaseClass implements C, D {}

    // Read until we get the class name
    phpLexerToken token;
    while(NextToken(token)) {
        if(token.IsAnyComment()) continue;
        if(token.type != kPHP_T_IDENTIFIER) {
            // expecting the class name
            return;
        }
        break;
    }

    // create new class entity
    PHPEntityBase::Ptr_t klass(new PHPEntityClass());
    klass->SetFilename(m_filename.GetFullPath());
    PHPEntityClass* pClass = klass->Cast<PHPEntityClass>();
    pClass->SetName(MakeIdentifierAbsolute(token.text));
    pClass->SetLine(token.lineNumber);

    // add the current class to the current scope
    CurrentScope()->AddChild(klass);
    m_scopes.push_back(klass);

    while(NextToken(token)) {
        if(token.IsAnyComment()) continue;
        switch(token.type) {
        case kPHP_T_EXTENDS: {
            // inheritance
            if(!ReadUntilFound(kPHP_T_IDENTIFIER, token)) return;
            pClass->SetExtends(MakeIdentifierAbsolute(token.text));
        } break;
        case kPHP_T_IMPLEMENTS: {
            wxArrayString implements;
            if(!ReadCommaSeparatedIdentifiers('{', implements)) return;
            pClass->SetImplements(implements);

        } break;
        case '{': {
            // entering the class body
            Parse(m_depth - 1);
            if(!m_reachedEOF) {
                m_scopes.pop_back();
            }
            return;
        }
        default:
            break;
        }
    }
}

bool PHPSourceFile::ReadCommaSeparatedIdentifiers(int delim, wxArrayString& list)
{
    phpLexerToken token;
    wxString temp;
    while(NextToken(token)) {
        if(token.IsAnyComment()) continue;
        if(token.type == delim) {
            if(!temp.IsEmpty() && list.Index(temp) == wxNOT_FOUND) {
                list.Add(MakeIdentifierAbsolute(temp));
            }
            UngetToken(token);
            return true;
        }

        switch(token.type) {
        case ',':
            if(list.Index(temp) == wxNOT_FOUND) {
                list.Add(MakeIdentifierAbsolute(temp));
            }
            temp.clear();
            break;
        default:
            temp << token.text;
            break;
        }
    }
    return false;
}

bool PHPSourceFile::ConsumeUntil(int delim)
{
    phpLexerToken token;
    while(NextToken(token)) {
        if(token.type == delim) {
            return true;
        }
    }
    return false;
}

bool PHPSourceFile::ReadExpression(wxString& expression)
{
    expression.clear();
    phpLexerToken token;
    int depth(0);
    while(NextToken(token)) {
        if(token.type == ';') {
            return true;

        } else if(token.type == '{') {
            UngetToken(token);
            return true;
        }

        switch(token.type) {
        case kPHP_T_C_COMMENT:
        case kPHP_T_CXX_COMMENT:
            // skip comments
            break;
        case '(':
            depth++;
            expression << "(";
            break;
        case ')':
            depth--;
            if(depth == 0) {
                expression << ")";
            }
            break;
        case kPHP_T_NEW:
            if(depth == 0) {
                expression << token.text << " ";
            }
            break;
        default:
            if(depth == 0) {
                expression << token.text;
            }
            break;
        }
    }
    // reached EOF
    return false;
}

void PHPSourceFile::UngetToken(const phpLexerToken& token)
{
    ::phpLexerUnget(m_scanner);
    // undo any depth / comments
    if(token.type == '{') {
        m_depth--;
    } else if(token.type == '}') {
        m_depth++;
    } else if(token.type == kPHP_T_C_COMMENT && !m_comments.empty()) {
        m_comments.erase(m_comments.begin() + m_comments.size() - 1);
    }
}

const PHPEntityBase* PHPSourceFile::Class()
{
    PHPEntityBase::Ptr_t curScope = CurrentScope();
    PHPEntityBase* pScope = curScope.get();
    while(pScope) {
        PHPEntityClass* cls = pScope->Cast<PHPEntityClass>();
        if(cls) {
            // this scope is a class
            return pScope;
        }
        pScope = pScope->Parent();
    }
    return NULL;
}

int PHPSourceFile::ReadUntilFoundOneOf(int delim1, int delim2, phpLexerToken& token)
{
    // loop until we find the open brace
    while(NextToken(token)) {
        if(token.type == delim1) {
            return delim1;
        } else if(token.type == delim2) {
            return delim2;
        }
    }
    return wxNOT_FOUND;
}