#ifndef WXCRAFTER_H
#define WXCRAFTER_H

#include <wx/settings.h>
#include <wx/xrc/xmlres.h>
#include <wx/xrc/xh_bmp.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

class SymbolViewTabPanelBaseClass : public wxPanel
{
protected:
    wxTextCtrl* m_textCtrlSearch;

protected:
    virtual void OnSearchSymbol(wxCommandEvent& event) { event.Skip(); }
    virtual void OnSearchEnter(wxCommandEvent& event) { event.Skip(); }

public:
    SymbolViewTabPanelBaseClass(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(-1,-1), long style = wxTAB_TRAVERSAL);
    virtual ~SymbolViewTabPanelBaseClass();
};

#endif