#include "php_parser_thread.h"
#include "PHPLookupTable.h"
#include "PHPSourceFile.h"

PHPParserThread* PHPParserThread::ms_instance = 0;

PHPParserThread::PHPParserThread()
{
}

PHPParserThread::~PHPParserThread() {}

PHPParserThread* PHPParserThread::Instance()
{
    if(ms_instance == 0) {
        ms_instance = new PHPParserThread();
    }
    return ms_instance;
}

void PHPParserThread::Release()
{
    ms_instance->Stop();
    if(ms_instance) {
        delete ms_instance;
    }
    ms_instance = 0;
}

void PHPParserThread::ProcessRequest(ThreadRequest* request)
{
    PHPParserThreadRequest* r = dynamic_cast<PHPParserThreadRequest*>(request);
    if(r) {
        // Now parse the files
        ParseFiles(r);
    }
}

void PHPParserThread::ParseFiles(PHPParserThreadRequest* request)
{
    const wxArrayString& files = request->files;
    wxFileName fnWorkspaceFile(request->workspaceFile);
    
    // Open the database
    PHPLookupTable lookuptable;
    lookuptable.Open(fnWorkspaceFile.GetPath());

    for(size_t i = 0; i < files.GetCount(); ++i) {
        PHPSourceFile sourceFile(wxFileName(files.Item(i)));
        sourceFile.SetFilename(files.Item(i));
        sourceFile.SetParseFunctionBody(false);
        sourceFile.Parse();

        // Update the database
        PHPLookupTable table;
        table.UpdateSourceFile(sourceFile);
    }
}
