// Plumbing shared by the pdf.* command sources: finding the document, snapshot
// undo, param specs and page/coordinate helpers.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "internal.hpp"

namespace wetwaresuite::pdf::tool {

// Runs an edit on a snapshot: undo restores the whole document.
class SnapshotEdit : public Action {
public:
    SnapshotEdit(std::shared_ptr<PdfDoc> doc, std::function<Result(PdfDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<PdfDoc> doc_;
    std::function<Result(PdfDoc&)> fn_;
    std::unique_ptr<DocData> before_;
};

using DocFn = std::function<Result(PdfDoc&, const Params&, const std::string& id)>;
ActionFactory docCommand(Session& session, DocFn fn, bool undoable);
ActionFactory plainCommand(std::function<Result(const Params&)> fn);

bool has(const Params& p, const std::string& key);

ParamSpec docSpec();
ParamSpec pageSpec(bool required = false);
ParamSpec str(const std::string& name, const std::string& description, bool required = false, Value fallback = {},
              std::vector<std::string> choices = {});
ParamSpec integer(const std::string& name, const std::string& description, bool required = false, Value fallback = {});
ParamSpec number(const std::string& name, const std::string& description, bool required = false, Value fallback = {});
ParamSpec flag(const std::string& name, const std::string& description, Value fallback = {});
ParamSpec list(const std::string& name, const std::string& description, bool required = false);

// 1-based page param -> 0-based index; throws when out of range
int pageParam(const DocData& d, const Params& p, const std::string& key = "page");
// "1-3,5", "all", "last" -> 0-based pages
std::vector<int> pagesParam(const DocData& d, const Params& p, const std::string& key = "pages");
Color colorParam(const Params& p, const std::string& key, Color fallback);
std::optional<Color> optColorParam(const Params& p, const std::string& key);
Font fontParam(const Params& p);

// a box given as shown now -> base page points
std::array<double, 4> boxToBase(const PageGeom& g, double x, double y, double w, double h);
std::array<double, 4> boxFromBase(const PageGeom& g, double x, double y, double w, double h);

Mark& addMark(DocData& d, int page, Mark m);
Mark* findMark(DocData& d, int id, int* page = nullptr);
Result markAdded(const DocData& d, int page, const Mark& m, const std::string& what);

// glyphs covered by a box (base page points), or matched by text
std::vector<int> charsInBox(const std::vector<TextChar>& chars, const std::array<double, 4>& box);
// text boxes per line for a set of chars (base page points): x, y, w, h
std::vector<std::array<double, 4>> lineBoxes(const std::vector<TextChar>& chars, const std::vector<int>& which);
// takes these glyphs out of the page's content; returns how many could not be (inside forms)
int dropGlyphs(DocData& d, int page, const std::vector<TextChar>& chars, const std::vector<int>& which);

std::shared_ptr<ImageData> loadPicture(const std::string& path);

void registerFileCommands(Session& s);
void registerEditCommands(Session& s);
void registerCoverCommands(Session& s);
void registerMarkCommands(Session& s);
void registerPageCommands(Session& s);
void registerSignCommands(Session& s);
void registerOcrCommands(Session& s);
std::shared_ptr<PdfDoc> findPdfDoc(Session& s, const Params& p, std::string& id, std::string& error);

}  // namespace wetwaresuite::pdf::tool
