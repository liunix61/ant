/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef RMLUI_CORE_ELEMENTDOCUMENT_H
#define RMLUI_CORE_ELEMENTDOCUMENT_H

#include "ElementDocument.h"
#include <set>

namespace Rml {

class ElementText;
class StyleSheet;
class DataModel;
class DataModelConstructor;
class Factory;

class Document {
public:
	Document(const Size& dimensions);
	virtual ~Document();

	bool Load(const std::string& path);

	/// Returns the source address of this document.
	const std::string& GetSourceURL() const;

	/// Sets the style sheet this document, and all of its children, uses.
	void SetStyleSheet(std::shared_ptr<StyleSheet> style_sheet);
	/// Returns the document's style sheet.
	const std::shared_ptr<StyleSheet>& GetStyleSheet() const;

	/// Creates the named element.
	/// @param[in] name The tag name of the element.
	ElementPtr CreateElement(const std::string& name);
	/// Create a text element with the given text content.
	/// @param[in] text The text content of the text element.
	TextPtr CreateTextNode(const std::string& text);

	virtual void LoadInlineScript(const std::string& content, const std::string& source_path, int source_line);
	virtual void LoadExternalScript(const std::string& source_path);

	void SetDimensions(const Size& dimensions);
	const Size& GetDimensions();
	Element* ElementFromPoint(Point pt) const;
	
	void Update();
	void Render();
	std::unique_ptr<ElementDocument> body;

public:
	DataModelConstructor CreateDataModel(const std::string& name);
	DataModelConstructor GetDataModel(const std::string& name);
	bool RemoveDataModel(const std::string& name);
	void UpdateDataModel(bool clear_dirty_variables);
	DataModel* GetDataModelPtr(const std::string& name) const;

private:
	using DataModels = std::unordered_map<std::string, std::unique_ptr<DataModel>>;
	DataModels data_models;

private:
	std::string source_url;
	std::shared_ptr<StyleSheet> style_sheet;
	Point mouse_position = Point(0,0);
	Size dimensions;
	bool dirty_dimensions = false;
	friend class Rml::Factory;
};

} // namespace Rml
#endif
