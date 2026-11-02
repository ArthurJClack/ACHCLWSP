# Office: spreadsheets, documents, slides and PDFs

`office` covers the everyday office work: budgets and lists in spreadsheets, letters and reports, slide decks, and PDF chores. It has four parts:

- **Spreadsheets** (`office.sheet_*`): an Excel-style formula engine with about 300 functions and dynamic arrays (XLOOKUP, SUMIFS, FILTER, SORT, UNIQUE, LET, finance and statistics), named ranges, number formats, cell styles (fonts, colours, borders, alignment), merged cells, frozen panes, conditional formatting with colour scales and data bars, data validation with dropdowns, sorting, filtering and autofilter, fill series, paste special, pivot tables, and charts placed in the sheet. It reads and writes `.xlsx`, `.ods` and `.csv` with all of that kept, and prints to PDF.
- **Documents** (`office.doc_*`): Word-style documents with paragraph styles (built-in and your own), character and paragraph formatting, numbered and bulleted lists, tables with shading and merged cells, images, headers and footers with page numbers, a table of contents, footnotes, comments and tracked changes, plus find and replace and word count. It reads and writes `.docx` and `.odt` with all of that kept, reads and writes Markdown and HTML, and exports PDF and PNG through its own layout engine.
- **Slide decks** (`office.deck_*`): PowerPoint-style decks with eight layouts and colour themes (or your own colours), text boxes, shapes, lines and pictures placed anywhere, a master with a logo, footer and slide numbers, speaker notes, transitions and timings, and hidden slides. Build them from a Markdown outline or one by one. They save as `.pptx` with all of that kept (PowerPoint, Keynote, Google Slides and LibreOffice open it) and export to PDF, notes pages and PNG. `office.deck_present` turns a deck into a slide show for any browser, with a presenter window.
- **PDF tools** (`office.pdf_*`): info, text extraction, merge, split, extract, delete or rotate pages, and images to PDF. Any PDF can also be rendered to PNG (this needs `pdftoppm` from poppler-utils). To view, mark up, edit, redact, fill in or sign a PDF, use the [PDF editor](pdf.md) that ships in the same app.

All of the formats above are handled inside the suite, with no other programs. `office.convert` converts between them in one step. When LibreOffice is installed it adds the rest: `.doc`, `.xls`, `.ppt`, `.rtf`, `.odp` output and more.

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands that read files and write files.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools office` serves just the office tools.

Every example below was run against the real binary, and the outputs are copied from those runs. Where an output was long it is shortened, and the cut is marked.

## How office works

**Documents and ids.** `office.sheet_new` and `office.sheet_open` open a workbook with an id such as `sheet1`. `office.doc_*` documents get ids like `doc1`, and decks get ids like `deck1`. The other commands take `doc=<id>`. When only one document of that kind is open you can leave `doc` out. Every change is one undo step, and the session's `undo`, `redo` and `history` commands work as everywhere else in the suite. `*_save` writes the document; the extension of `path` picks the format. `*_export` writes another format without changing which file the document belongs to.

**One-shot commands with `file=`.** The commands that only read take `file=` instead of `doc=`: they load the file, answer, and forget it. This works for info, get, find, calc, preview, chart, word count and export. It is how the CLI examples work without a batch script:

```sh
wetwaresuite office.sheet_calc file=orders.csv formula="SUMIF(D:D,\"shipped\",C:C)"
wetwaresuite office.doc_word_count file=report.docx
wetwaresuite office.deck_export file=talk.md path=talk.png
```

**Several lines in one argument.** CSV text, bullets, list items and table rows have one item per line. A command line cannot hold a real line break, so write `\n` and it becomes one. A batch file takes the same `\n`, and `\"` for a quote inside a quoted value. Over MCP, Claude sends real line breaks. Every such parameter also has a `_file` twin (`csv_file=`, `text_file=`, `bullets_file=`, `markdown_file=`, `outline_file=`) that reads the text from a file instead.

**Previews.** Each kind has a preview command (`office.sheet_preview`, `office.doc_preview`, `office.deck_preview`) that returns a PNG drawn exactly as the PDF export draws it. Most editing commands also take `preview=true`, which attaches a picture of the result. Claude uses these to check its own work.

**In the window.** `wetwaresuite-office` (or `wetwaresuite-gui`) opens spreadsheets as an editable grid: click a cell, type a value or `=formula`, press Enter. Each cell you type is an `office.sheet_set` (or `office.sheet_clear`) step, so it undoes the same way as a script. The grid shows cell colours, bold, underline, alignment, merged cells, conditional formats and data bars. Every other command in this page is in the window too: press **Ctrl+K**, type what you want (`style`, `merge`, `pivot`, ...) and fill in its form. See [the window tutorial](gui.md#spreadsheets-type-into-the-cells).

Documents and slides are typed into right on the page. Click anywhere in a paragraph, heading, list or table and it turns into a text box in place, with the caret on the letter you clicked. Ctrl+B, Ctrl+I and Ctrl+U format the selection, Enter splits the paragraph, and Esc or a click elsewhere keeps what you typed. Click a task box (`- [ ]` in Markdown) to tick it. Ctrl+click a `[[note link]]` to open that note, which is made if it doesn't exist yet. On a slide, click the title, the bullets or a text box and type over it in the slide's own colours and size. In the spreadsheet grid, drag to pick a range (Shift+click extends it). Drag the small square at its corner to fill a series down or across. Delete clears the range, Ctrl+C copies it, and the bar above the grid has bold, italic, underline, strikethrough, fill and text colours, alignment, number formats and **Freeze**. Frozen rows and columns stay put while the rest scrolls.

Layouts (`.wwlay`) show the page with every frame outlined. Click a frame to pick it, drag it to move it, or drag a handle to resize it. A ghost shows where it lands, with its position and size in mm. Double-click a text frame to retype it (it keeps its font, size and colour). Arrow keys nudge it by 1 mm (with Shift, 0.1 mm), Delete removes it, and PageUp or PageDown changes the page. The same steps are `layout.frame_set`, `layout.frame_text` and `layout.frame_delete` for scripts.

**Built in or LibreOffice.** `wetwaresuite office.formats` shows what is built in, and whether LibreOffice and pdftoppm were found:

```sh
$ wetwaresuite office.formats
spreadsheets (office.sheet_*): read csv ods tab tsv xlsm xlsx; write csv html ods pdf tsv xlsx
  through LibreOffice: read dbf fods numbers sxc xls xlsb xlt xltx; write fods xls
documents (office.doc_*): read docx htm html markdown md odt txt; write docx htm html markdown md odt pdf txt
  through LibreOffice: read doc docm dot dotx fodt lwp pages rtf sxw wpd wps; write doc epub fodt rtf
slide decks (office.deck_*): read markdown md pptx; write markdown md pdf pptx png
  through LibreOffice: read fodp key odp potx pps ppsx ppt pptm sxi; write fodp odp ppt
pdf (office.pdf_*): info, text, merge, split, extract, delete pages, rotate, images to pdf (built in); render pages to png (pdftoppm)
LibreOffice: installed; pdftoppm: installed
libreoffice: true
pdftoppm: true
```

The LibreOffice formats work in every open, save and export command, not only in `office.convert`. The suite runs LibreOffice in the background with its own profile, so it doesn't disturb a LibreOffice window you have open. Install it with your package manager (`libreoffice` on Linux, `brew install --cask libreoffice` on macOS, or the installer from libreoffice.org on Windows). Install `poppler-utils` (or `brew install poppler`) for `office.pdf_render`. Everything else works without either program.

## Task 1: a budget spreadsheet with formulas and a chart

You want a quarterly budget with monthly spending per category, totals across and down, a column that flags categories over plan, currency formatting, a bar chart and a pie chart, saved as `.xlsx` and printed to PDF.

**CLI.** A CSV file can hold formulas: any cell that starts with `=` is a formula when the suite reads it. Write the numbers and formulas in `budget.csv`:

```text
Category,Jan,Feb,Mar,Total
Rent,1200,1200,1200,=SUM(B2:D2)
Groceries,420,385,450,=SUM(B3:D3)
Transport,95,110,80,=SUM(B4:D4)
Fun,150,90,200,=SUM(B5:D5)
Total,=SUM(B2:B5),=SUM(C2:C5),=SUM(D2:D5),=SUM(E2:E5)
```

Then convert it, look at the results, chart it, and ask a question without changing anything:

```sh
$ wetwaresuite office.convert input=budget.csv output=budget.xlsx
converted budget.csv to budget.xlsx (built-in, 1 sheet(s), 5 KB)
budget.xlsx
output: budget.xlsx
backend: built-in
bytes: 5469

$ wetwaresuite office.sheet_get file=budget.xlsx
A1:E6 (6 rows x 5 cols)
	A	B	C	D	E
1	Category	Jan	Feb	Mar	Total
2	Rent	1200	1200	1200	3600
3	Groceries	420	385	450	1255
4	Transport	95	110	80	285
5	Fun	150	90	200	440
6	Total	1865	1785	1930	5580
range: A1:E6
rows: 6
cols: 5

$ wetwaresuite office.sheet_chart file=budget.xlsx range=A1:D5 title="Spending by month" path=budget-chart.png
bar chart of A1:D5: 3 series x 4 categories (Jan, Feb, Mar); wrote budget-chart.png
budget-chart.png
path: budget-chart.png
series: 3
categories: 4
attachment: chart.png (29436 bytes)

$ wetwaresuite office.sheet_calc file=budget.xlsx formula="AVERAGE(E2:E5)"
=AVERAGE(E2:E5) -> 1395
1395
value: 1395
type: number
```

The `.xlsx` keeps the formulas, and their computed values are stored alongside them, so Excel, LibreOffice and Google Sheets show the numbers straight away. `sheet_chart` reads the usual layout by itself: the header row names the series (Jan, Feb, Mar) and the first column names the categories. Add `by_rows=true` to swap the two.

**Batch.** A script builds the same budget step by step and adds formatting:

```text
# budget.txt - a quarterly budget with totals, a check column and a chart
office.sheet_new name=Budget -> b

# the numbers: one row per line (\n is a line break)
office.sheet_set doc=$b range=A1 csv="Category,Jan,Feb,Mar\nRent,1200,1200,1200\nGroceries,420,385,450\nTransport,95,110,80\nFun,150,90,200"

# totals across and down; the formula is copied with relative refs
office.sheet_set doc=$b range=E1 value=Total
office.sheet_set doc=$b range=E2:E5 value="=SUM(B2:D2)"
office.sheet_set doc=$b range=F1 value="vs plan"
office.sheet_set doc=$b range=F2:F5 value="=IF(E2>1500,\"over\",\"ok\")"
office.sheet_set doc=$b range=A6 value=Total
office.sheet_set doc=$b range=B6:E6 value="=SUM(B2:B5)"
office.sheet_set doc=$b range=A7 value="Monthly average"
office.sheet_set doc=$b range=B7 value="=ROUND(AVERAGE(B6:D6),2)"

# looks
office.sheet_format doc=$b range=B2:E7 format=currency
office.sheet_format doc=$b range=A1:F1 bold=true
office.sheet_format doc=$b range=A6:E6 bold=true
office.sheet_format doc=$b range=A:F autofit=true

office.sheet_get doc=$b show=both
office.sheet_chart doc=$b range=A1:D5 type=bar title="Spending by month" path=budget-chart.png
office.sheet_chart doc=$b range=A1:E5 series=Total type=pie title="Where Q1 went" path=budget-pie.png
office.sheet_save doc=$b path=budget-q1.xlsx
office.sheet_save doc=$b path=budget-q1.pdf
```

```sh
$ wetwaresuite batch budget.txt
created sheet1: 1 sheet: Budget (empty)
...
set A1:D5 (20 cells); A1 = Category
...
set E2:E5 (4 cells); E2 = 3600
...
set F2:F5 (4 cells); F2 = over
...
set B7; B7 = 1860
...
formatted B2:E7: format $#,##0.00
formatted A1:F1: bold
formatted A6:E6: bold
formatted A:F: autofit widths
A1:F7 (7 rows x 6 cols)
	A	B	C	D	E	F
1	Category	Jan	Feb	Mar	Total	vs plan
2	Rent	$1,200.00	$1,200.00	$1,200.00	$3,600.00 [=SUM(B2:D2)]	over [=IF(E2>1500,"over","ok")]
3	Groceries	$420.00	$385.00	$450.00	$1,255.00 [=SUM(B3:D3)]	ok [=IF(E3>1500,"over","ok")]
4	Transport	$95.00	$110.00	$80.00	$285.00 [=SUM(B4:D4)]	ok [=IF(E4>1500,"over","ok")]
5	Fun	$150.00	$90.00	$200.00	$440.00 [=SUM(B5:D5)]	ok [=IF(E5>1500,"over","ok")]
6	Total	$1,865.00 [=SUM(B2:B5)]	$1,785.00 [=SUM(C2:C5)]	$1,930.00 [=SUM(D2:D5)]	$5,580.00 [=SUM(E2:E5)]	
7	Monthly average	$1,860.00 [=ROUND(AVERAGE(B6:D6),2)]				
...
bar chart of A1:D5: 3 series x 4 categories (Jan, Feb, Mar); wrote budget-chart.png
...
pie chart of A1:E5: 1 series x 4 categories (Total); wrote budget-pie.png
...
saved budget-q1.xlsx
...
saved budget-q1.pdf
```

(Shortened: each command also prints its `key: value` fields.) Some things to notice:

- `value="=SUM(B2:D2)"` over `E2:E5` works like dragging a formula down in Excel: E3 gets `=SUM(B3:D3)`, and so on. Put `$` in a reference (`$B$2`) to keep it fixed.
- `csv=` pastes a block starting at the top-left cell of `range`. Numbers, `$1,200`, `15%`, `2026-03-01` dates and `TRUE` become typed values. Use `value=` for formulas you want copied across a range.
- `show=both` puts each formula in brackets after its value. `show=formulas` lists what you would type to rebuild the sheet.
- `series=Total` picks one column for the pie chart. The pie's legend shows each share as a percentage.
- The PDF is a printable grid with the number formats and bold applied. Wide sheets continue on more A4 landscape pages.

To change a number later, open the file, set the cell and save. Every formula that depends on it is recalculated:

```text
# change.txt
office.sheet_open path=budget-q1.xlsx -> b
office.sheet_set doc=$b range=C3 value=410
office.sheet_get doc=$b range=E3
office.sheet_save doc=$b
```

```sh
$ wetwaresuite batch change.txt
opened sheet1: 1 sheet: Budget A1:F7 - budget-q1.xlsx; 13 formulas
...
set C3; C3 = $410.00
...
E3 = $1,280.00  (formula =SUM(B3:D3))
type number, format $#,##0.00
reads B3:D3
used by F3, E6
...
saved budget-q1.xlsx
```

`office.sheet_get` on one cell also lists the cells it reads from and the cells that read it, which helps when you're tracing a wrong total.

**Claude.**

> Make me a Q1 budget spreadsheet in /home/me/money/budget-q1.xlsx: rent 1200 every month, groceries 420/385/450, transport 95/110/80, fun 150/90/200. Totals per category and per month, flag anything over 1500 for the quarter, currency format, and a bar chart of spending by month next to it as a PNG.

Claude calls:

1. `office_sheet_new`, then `office_sheet_set` with `csv` for the numbers.
2. `office_sheet_set` with `value="=SUM(B2:D2)"` over `E2:E5`, and the same for the totals row and the `IF` column.
3. `office_sheet_format` with `format=currency`, `bold` and `autofit`.
4. `office_sheet_get` with `show=both` to check the values, or `office_sheet_preview` to look at the sheet as a picture.
5. `office_sheet_chart`, which returns the chart image so Claude sees what it made.
6. `office_sheet_save`.

If Claude isn't sure a formula is right, `office_sheet_calc` with `formula=` evaluates it without writing anything.

## Task 2: clean up a CSV export and join it with VLOOKUP

An online shop exported `orders.csv`. The customer ids have stray spaces and mixed case, the status column is spelled three ways, and the amounts are text with dollar signs:

```text
Order,Customer,Amount,Status
1001, c-002 ,"$1,250.00",shipped
1002,C-001,$310.50,SHIPPED
1003,c-004,"$2,400.00",pending
1004,C-003,$95.00,Shipped
1005,c-002,$780.25,cancelled
1006,C-005,"$1,020.00",shipped
1007, C-001,$45.99,pending
1008,c-003,$660.00,shipped
```

Customer names and regions are in another file, `customers.csv`:

```text
ID,Name,Region
C-001,Acme Ltd,North
C-002,Birch & Co,South
C-003,Cobalt Inc,North
C-004,Delta GmbH,East
```

You want a clean table with a proper id, the customer's name and region, one spelling of the status, without the cancelled orders and with the biggest orders first, plus revenue per region.

**CLI.** Look at the data first and ask questions straight from the CSV:

```sh
$ wetwaresuite office.sheet_info file=orders.csv
orders.csv: 1 sheet(s)
  1. orders (active): A1:D9 (9 rows x 4 cols), 36 cells, 0 formulas
     row 1: Order | Customer | Amount | Status
doc: orders.csv
sheets: 1
active: orders
formulas: 0
errors: 0
path: orders.csv

$ wetwaresuite office.sheet_calc file=orders.csv formula="SUMIF(D:D,\"shipped\",C:C)"
=SUMIF(D:D,"shipped",C:C) -> 3335.5
3335.5
value: 3335.5
type: number

$ wetwaresuite office.sheet_find file=orders.csv query=c-002
2 cell(s) match "c-002":
  orders!B2:  c-002 
  orders!B6: c-002
2
count: 2
first: orders!B2

$ wetwaresuite office.convert input=orders.csv output=orders.xlsx
converted orders.csv to orders.xlsx (built-in, 1 sheet(s), 6 KB)
...
```

The amounts were read as numbers (`"$1,250.00"` becomes 1250), and criteria such as `"shipped"` ignore case like Excel's do, so the total covers `SHIPPED` and `Shipped` too. The delimiter is detected, so semicolon-separated files from European Excel work as well (`delimiter=` overrides it).

**Batch.** The cleanup needs several steps, so it goes in a script:

```text
# cleanup.txt - tidy an orders export and join customer names onto it
office.sheet_open path=orders.csv -> o
office.sheet_add doc=$o name=Customers csv_file=customers.csv

# 1. a clean customer id next to the messy one: trimmed and upper-case
office.sheet_insert_cols doc=$o sheet=orders at=C
office.sheet_set doc=$o range=orders!C1 value=ID
office.sheet_set doc=$o range=orders!C2:C9 value="=UPPER(TRIM(B2))"

# 2. the join: VLOOKUP the name, XLOOKUP the region (with a default for unknown ids)
office.sheet_set doc=$o range=orders!F1 value=Name
office.sheet_set doc=$o range=orders!F2:F9 value="=IFERROR(VLOOKUP(C2,Customers!A:C,2,FALSE),\"unknown\")"
office.sheet_set doc=$o range=orders!G1 value=Region
office.sheet_set doc=$o range=orders!G2:G9 value="=XLOOKUP(C2,Customers!A:A,Customers!C:C,\"?\")"

# 3. one spelling for the status column
office.sheet_set doc=$o range=orders!H1 value=State
office.sheet_set doc=$o range=orders!H2:H9 value="=LOWER(E2)"

office.sheet_format doc=$o range=orders!A1:H1 bold=true
office.sheet_format doc=$o range=orders!A:H autofit=true
office.sheet_get doc=$o sheet=orders

# 4. drop the cancelled orders, biggest first
office.sheet_filter doc=$o sheet=orders column=State criteria="<>cancelled" mode=delete
office.sheet_sort doc=$o sheet=orders by="Amount desc"

# 5. totals per region on their own sheet
office.sheet_add doc=$o name=Summary csv="Region,Orders,Revenue\nNorth\nSouth\nEast"
office.sheet_set doc=$o range=Summary!B2:B4 value="=COUNTIF(orders!G:G,A2)"
office.sheet_set doc=$o range=Summary!C2:C4 value="=SUMIF(orders!G:G,A2,orders!D:D)"
office.sheet_format doc=$o range=Summary!C2:C4 format=currency
office.sheet_get doc=$o sheet=Summary
office.sheet_find doc=$o query=unknown

office.sheet_save doc=$o path=orders-clean.xlsx
office.sheet_save doc=$o path=orders-clean.csv sheet=orders
```

```sh
$ wetwaresuite batch cleanup.txt
opened sheet1: 1 sheet: orders A1:D9 - orders.csv
...
added sheet Customers at position 2
...
inserted column C in orders; formulas adjusted
...
set orders!C2:C9 (8 cells); C2 = C-002
...
set orders!F2:F9 (8 cells); F2 = Birch & Co
...
orders!A1:H9 (9 rows x 8 cols)
	A	B	C	D	E	F	G	H
1	Order	Customer	ID	Amount	Status	Name	Region	State
2	1001	 c-002 	C-002	$1,250.00	shipped	Birch & Co	South	shipped
3	1002	C-001	C-001	$310.50	SHIPPED	Acme Ltd	North	shipped
4	1003	c-004	C-004	$2,400.00	pending	Delta GmbH	East	pending
5	1004	C-003	C-003	$95.00	Shipped	Cobalt Inc	North	shipped
6	1005	c-002	C-002	$780.25	cancelled	Birch & Co	South	cancelled
7	1006	C-005	C-005	$1,020.00	shipped	unknown	?	shipped
8	1007	 C-001	C-001	$45.99	pending	Acme Ltd	North	pending
9	1008	c-003	C-003	$660.00	shipped	Cobalt Inc	North	shipped
...
7 of 8 rows match H <>cancelled; deleted the other 1 rows
...
sorted 7 rows of orders!A1:H8 by D desc (header kept)
...
Summary!A1:C4 (4 rows x 3 cols)
	A	B	C
1	Region	Orders	Revenue
2	North	4	$1,111.49
3	South	1	$1,250.00
4	East	1	$2,400.00
...
1 cell(s) match "unknown":
  orders!F4: unknown  (=IFERROR(VLOOKUP(C4,Customers!A:C,2,FALSE),"unknown"))
...
saved orders-clean.xlsx
...
saved orders-clean.csv (sheet orders only; CSV holds one sheet) (formulas saved as their values)
```

Some things to notice:

- `csv_file=` reads a whole file into a new sheet, so the two exports end up in one workbook and formulas can refer across them (`Customers!A:C`).
- The amounts kept their `$1,250.00` look: text read as money gets a currency format along with its number.
- `office.sheet_filter` with `mode=delete` removes the rows that don't match, in place. The default `mode=copy` writes the matching rows to a new sheet instead, and `mode=count` only counts. Criteria work like COUNTIF: `>100`, `<>done`, `a*`.
- `office.sheet_sort` takes header names or column letters (`by="Region, Amount desc"`). The header row stays on top, and formulas move with their rows.
- Customer `C-005` isn't in the customer list. `IFERROR` (or XLOOKUP's fourth argument) turns the `#N/A` into a readable value, and `office.sheet_find` then finds the row (row 4 after the sort).
- A CSV holds one sheet and no formulas, so the CSV save writes the `orders` sheet's computed values. The `.xlsx` keeps all three sheets with their formulas.

**Claude.**

> /home/me/shop/orders.csv is a messy export: customer ids have spaces and mixed case, statuses are spelled differently. Join on the customer names and regions from /home/me/shop/customers.csv, drop cancelled orders, sort biggest first, and add a sheet with order count and revenue per region. Tell me which orders have a customer id that isn't in the list. Save it as orders-clean.xlsx next to the input.

Claude calls:

1. `office_sheet_open` and `office_sheet_get`, to see the columns and the mess.
2. `office_sheet_add` with `csv_file` for the customer list.
3. `office_sheet_insert_cols`, then `office_sheet_set` with `=UPPER(TRIM(B2))` down the new column.
4. `office_sheet_set` with the `VLOOKUP`/`XLOOKUP` formulas, and `office_sheet_find` with `query=unknown` to answer your question.
5. `office_sheet_filter` with `mode=delete`, then `office_sheet_sort`.
6. `office_sheet_add` for the summary, with `COUNTIF`/`SUMIF` formulas.
7. `office_sheet_save`.

## Task 3: a report document exported to PDF

You want a short report with headings, a bold total, a bulleted list, a table, the chart from task 1, a quote, and a second page with numbered next steps. You want it as a PDF to send and a `.docx` for colleagues who use Word.

**CLI.** The quickest way is to write it in Markdown, `q1-budget.md`:

```markdown
---
title: Q1 household budget
author: Sam Rivera
---

# Q1 household budget

Spending came to **$5,580** over the quarter, about *$1,860 a month*. Rent is
fixed; groceries and fun moved the most. The raw numbers are in
[budget-q1.xlsx](budget-q1.xlsx).

## Highlights

- Rent stayed at $1,200 a month
- Groceries peaked in March at $450
- Fun spending more than doubled from February to March
    - one concert, one weekend away

## By category

| Category  | Q1 total | Share |
|-----------|---------:|------:|
| Rent      |   $3,600 |   65% |
| Groceries |   $1,255 |   22% |
| Transport |     $285 |    5% |
| Fun       |     $440 |    8% |

![Spending by month](budget-chart.png)

> Next quarter: cap fun at $150 a month and plan groceries weekly.

<!-- pagebreak -->

## Next steps

1. Review the grocery list every Sunday
2. Move $200 a month into savings
3. Check the numbers again in July
```

Then convert it:

```sh
$ wetwaresuite office.convert input=q1-budget.md output=q1-budget.pdf
converted q1-budget.md to q1-budget.pdf (built-in, 11 blocks, 27 KB)
q1-budget.pdf
output: q1-budget.pdf
backend: built-in
bytes: 27937

$ wetwaresuite office.convert input=q1-budget.md output=q1-budget.docx
converted q1-budget.md to q1-budget.docx (built-in, 11 blocks, 46 KB)
...

$ wetwaresuite office.doc_word_count file=q1-budget.md
106 words, 582 characters (504 without spaces), 14 paragraphs, 2 page(s); about 1 min to read
106
words: 106
characters: 582
characters_no_spaces: 504
paragraphs: 14
pages: 2

$ wetwaresuite office.doc_export file=q1-budget.md path=q1-budget-page1.png dpi=80
exported page 1 of 2 to q1-budget-page1.png
q1-budget-page1.png
path: q1-budget-page1.png

$ wetwaresuite office.convert input=q1-budget.docx output=q1-budget.odt
converted q1-budget.docx to q1-budget.odt (built-in + LibreOffice, 11 blocks, 51 KB)
...
```

The PDF is laid out by the suite itself. It uses the standard PDF fonts (Helvetica, Times and Courier), which every PDF reader has, so the files stay small. It has wrapped paragraphs, real bold and italic, clickable links, bullets with sub-levels, a table with a shaded header row, the image scaled to the text width, a page break and "1 / 2" page numbers. The front matter becomes the PDF's title and author. `.odt` isn't written by the suite, so that conversion goes through LibreOffice ("built-in + LibreOffice": read here, written by LibreOffice).

**Batch.** To build the document piece by piece, or to edit an existing `.docx`, use the block commands. A document is a list of numbered blocks; `office.doc_info` lists them, and `doc_edit`, `doc_delete`, `doc_move` and `doc_add at=` use those numbers:

```text
# writeup.txt - build a two-page write-up block by block, then export it
office.doc_new title="Q1 household budget" author="Sam Rivera" font=serif -> d

office.doc_add doc=$d kind=heading text="Q1 household budget"
office.doc_add doc=$d text="Spending came to **$5,580** over the quarter, about *$1,860 a month*."
office.doc_add doc=$d kind=heading level=2 text=Highlights
office.doc_add doc=$d kind=list text="Rent stayed at $1,200 a month\nGroceries peaked in March\nFun spending doubled\n  one concert, one weekend away"
office.doc_add doc=$d kind=heading level=2 text="By category"
office.doc_add doc=$d kind=table text="Category | Q1 total\nRent | $3,600\nGroceries | $1,255\nTransport | $285\nFun | $440"
office.doc_add doc=$d kind=image path=budget-chart.png width=400 alt="Spending by month"
office.doc_add doc=$d kind=pagebreak
office.doc_add doc=$d kind=heading level=2 text="Next steps"
office.doc_add doc=$d kind=numbered text="Review the grocery list every Sunday\nMove $200 a month into savings"

# fix things after the fact
office.doc_info doc=$d
office.doc_edit doc=$d block=2 text="Spending came to **$5,580** over the quarter, about *$1,860 a month*. Rent is fixed."
office.doc_add doc=$d kind=quote at=8 text="Next quarter: cap fun at $150 a month."
office.doc_find_replace doc=$d find="Fun spending doubled" replace="Fun spending more than doubled"
office.doc_word_count doc=$d

office.doc_setup doc=$d page=letter margin=20
office.doc_export doc=$d path=writeup.pdf
office.doc_save doc=$d path=writeup.docx
office.doc_export doc=$d path=writeup.html
office.doc_export doc=$d path=writeup-p1.png page=1 dpi=80
```

```sh
$ wetwaresuite batch writeup.txt
created doc1: "Q1 household budget", 0 blocks
...
added 4. list (4 items): Rent stayed at $1,200 a month / Groceries peaked in March / Fun spending doubled / one con...
...
added 6. table (5x2): Category | Q1 total / Rent | $3,600 / Groceries | $1,255 / Transport | $285 / Fun | $440
...
added 7. image (800x500 px): Spending by month
...
doc1: "Q1 household budget" by Sam Rivera
a4, margins 25.4 mm, serif 11 pt; 2 page(s), 60 words, 10 blocks:
  1. heading 1: Q1 household budget
  2. paragraph: Spending came to $5,580 over the quarter, about $1,860 a month.
  3. heading 2: Highlights
  4. list (4 items): Rent stayed at $1,200 a month / Groceries peaked in March / Fun spending doubled / one con...
  5. heading 2: By category
  6. table (5x2): Category | Q1 total / Rent | $3,600 / Groceries | $1,255 / Transport | $285 / Fun | $440
  7. image (800x500 px): Spending by month
  8. page_break
  9. heading 2: Next steps
  10. numbered list (2 items): Review the grocery list every Sunday / Move $200 a month into savings
...
block 2. paragraph: Spending came to $5,580 over the quarter, about $1,860 a month. Rent is fixed.
added 8. quote: Next quarter: cap fun at $150 a month.
...
replaced 1 match(es) of "Fun spending doubled" with "Fun spending more than doubled" in block(s) 4
...
73 words, 406 characters (355 without spaces), 13 paragraphs, 2 page(s); about 1 min to read
...
page setup: 612 x 792 pt, margins 20 mm, serif 11 pt; 2 page(s)
exported writeup.pdf (2 pages)
...
saved writeup.docx
...
exported writeup.html
...
exported page 1 of 2 to writeup-p1.png
```

Some things to notice:

- Paragraphs, headings and quotes take inline Markdown: `**bold**`, `*italic*`, `<u>underline</u>`, `` `code` `` and `[links](https://...)`. Lists take one item per line, and two spaces of indent make a sub-item. Table cells are split by `|`, tabs or commas, and the first row is the header.
- `kind=markdown` adds a whole Markdown text as blocks. That is handy for pasting a section in the middle of a `.docx`.
- `doc_find_replace` keeps each run's style, and it searches lists and table cells too.
- `.html` is one self-contained file with the images inside it. A `.md` save puts its images in a `<name>_media/` folder next to it.
- To edit a `.docx` someone sent you, `office.doc_open` it, change blocks, and `office.doc_save` it. Styles, formatting, lists, tables, images, headers and footers, footnotes, comments, tracked changes and the table of contents survive the round trip (Task 8 shows them); the limits section lists what doesn't.

**Claude.**

> Write a one-page summary of my Q1 budget as a PDF at /home/me/money/q1-report.pdf: a heading, two sentences with the total in bold, highlights as bullets, a table of the category totals, and the chart /home/me/money/budget-chart.png. Put the next steps on a second page. Also save a .docx copy.

Claude usually writes the whole thing as Markdown in one `office_doc_new` call (`markdown=` with real line breaks). It then calls `office_doc_preview` to look at page 1 and `office_doc_info` to check the page count. It fixes things with `office_doc_edit` or `office_doc_setup` (for example `margin=20` when the content just spills onto another page), and finishes with `office_doc_export` to `.pdf` and `office_doc_save` to `.docx`. To revise an existing file it calls `office_doc_open`, `office_doc_get_text` (Markdown with block numbers) and then the block commands.

## Task 4: a 5-slide deck from bullet points

You have notes for a short talk and want slides: a title slide, two content slides (one with two columns), a numbered plan and a closing slide, in a teal theme with speaker notes. You want a `.pptx` to present from and a PDF to hand out.

**CLI.** Write the outline in Markdown, `talk.md`. Each `#` heading starts a slide:

```markdown
---
title: Moving to the new build system
author: Platform team
theme: teal
---

# Moving to the new build system
Platform team, October 2026

# Why change
- Clean builds take 14 minutes
- Nobody can explain the old scripts
  - three generations of Makefiles
- CI cost doubled this year
> Start with the CI bill, it gets attention.

<!-- layout: two_column -->
# Old vs new
- 14 min clean build
- Hand-written rules
<!-- column -->
- 3 min clean build
- One CMake file per target

# Plan
1. Port the core libraries in November
2. Port the tools in December
3. Delete the Makefiles in January

# Questions?
build-team@example.com
```

```sh
$ wetwaresuite office.deck_info file=talk.md
talk.md: "Moving to the new build system", 5 slide(s), theme teal, 16:9
  1. title: "Moving to the new build system" / Platform team, October 2026
  2. content: "Why change", 4 bullet(s), notes
  3. two_column: "Old vs new", 2 bullet(s) + 2 right
  4. content: "Plan", 3 bullet(s)
  5. section: "Questions?" / build-team@example.com
doc: talk.md
slides: 5
theme: teal

$ wetwaresuite office.convert input=talk.md output=talk.pptx
converted talk.md to talk.pptx (built-in, 5 slides, 41 KB)
...

$ wetwaresuite office.convert input=talk.md output=talk.pdf
converted talk.md to talk.pdf (built-in, 5 slides, 4 KB)
...

$ wetwaresuite office.deck_export file=talk.md path=talk.png width=640
exported 5 slides as talk-1.png ... talk-5.png
talk-1.png
files: 5
```

The layouts were worked out from the content. A first slide without bullets became the title slide, a closing slide with only a title and a line became a section slide, and the rest are content slides. The `1.` items stay numbered. The `>` line is the speaker notes, which appear in PowerPoint's notes pane. Title and section slides use the theme colour as background; content slides have the title in the theme colour over a short accent bar, and a slide number. When a slide has too many bullets the text shrinks to fit, and `office.deck_info` warns about slides where it still can't.

The outline syntax:

| Line | Meaning |
| --- | --- |
| front matter `title:`, `author:`, `theme:`, `font:`, `aspect:` | deck settings (`aspect: 4:3` for old projectors) |
| `# Heading` | starts a slide with that title (`---` on its own line also starts one) |
| `- item`, `* item`, `1. item` | bullets; indent two spaces per level, up to five levels |
| `> text` or `Notes: text` | speaker notes |
| `![](picture.png)` | the slide's picture, relative to the outline file |
| other lines | the subtitle |
| `<!-- layout: name -->` | picks the next slide's layout: `title`, `section`, `content`, `two_column`, `image`, `image_right`, `title_only`, `blank` |
| `<!-- column -->` | starts the right column of a `two_column` slide |

Markdown is read as a slide outline when it has deck markers (`theme:` or `aspect:` in the front matter, or layout comments), or when the output is a deck format such as `.pptx`. For anything else, `office.convert kind=deck` or `kind=doc` decides.

**Batch.** To build the deck slide by slide, or to change an existing `.pptx`:

```text
# talk.txt - a five-slide deck built slide by slide
office.deck_new title="Moving to the new build system" theme=teal -> t

office.deck_add_slide doc=$t layout=title title="Moving to the new build system" subtitle="Platform team, October 2026"
office.deck_add_slide doc=$t title="Why change" bullets="Clean builds take 14 minutes\nNobody can explain the old scripts\n  three generations of Makefiles\nCI cost doubled this year" notes="Start with the CI bill."
office.deck_add_slide doc=$t layout=two_column title="Old vs new" bullets="14 min clean build\nHand-written rules" right="3 min clean build\nOne CMake file per target"
office.deck_add_slide doc=$t title="Plan" bullets="1. Port the core libraries in November\n2. Port the tools in December\n3. Delete the Makefiles in January"
office.deck_add_slide doc=$t layout=section title="Questions?" subtitle="build-team@example.com"

# a picture next to the bullets, then a different colour
office.deck_edit_slide doc=$t slide=2 image=budget-chart.png
office.deck_theme doc=$t theme=#5b3cc4
office.deck_info doc=$t

office.deck_save doc=$t path=talk-built.pptx
office.deck_export doc=$t path=talk-built.pdf
office.deck_export doc=$t path=talk-built-2.png slide=2 width=960
office.deck_export doc=$t path=talk-built.md
```

```sh
$ wetwaresuite batch talk.txt
created deck1: "Moving to the new build system", 0 slides, theme teal
...
added slide 1. title: "Moving to the new build system" / Platform team, October 2026
...
added slide 2. content: "Why change", 4 bullet(s), notes
...
added slide 3. two_column: "Old vs new", 2 bullet(s) + 2 right
...
added slide 4. content: "Plan", 3 bullet(s)
...
added slide 5. section: "Questions?" / build-team@example.com
...
slide 2. content: "Why change", 4 bullet(s), image 800x500, notes
theme #5b3cc4
deck1: "Moving to the new build system", 5 slide(s), theme #5b3cc4, 16:9
  1. title: "Moving to the new build system" / Platform team, October 2026
  2. content: "Why change", 4 bullet(s), image 800x500, notes
  3. two_column: "Old vs new", 2 bullet(s) + 2 right
  4. content: "Plan", 3 bullet(s)
  5. section: "Questions?" / build-team@example.com
...
saved talk-built.pptx (5 slides)
...
exported talk-built.pdf (5 slides)
...
exported slide 2 to talk-built-2.png
...
exported talk-built.md (5 slides)
```

Some things to notice:

- A content slide with a picture puts the picture to the right of the bullets. `layout=image` makes the picture fill the slide under the title instead.
- The themes are `blue` (the default), `teal`, `green`, `orange`, `red`, `purple`, `slate` (light grey background) and `dark` (dark background). Any colour such as `#5b3cc4` works as the accent. `font=serif` or `font=mono` changes every slide's font.
- Exporting to `.md` writes the deck back as an outline, with its pictures in `talk-built_media/`. You can edit that file as text and convert it again.
- `office.deck_open` reads `.pptx` files from PowerPoint, Google Slides and LibreOffice: titles, subtitles, bullets with levels, pictures, text boxes, shapes, speaker notes, transitions and the theme colours. Then `deck_edit_slide`, `deck_move_slide` and `deck_delete_slide` change them. Titles and bullets are laid out again in the suite's own layouts, so a heavily designed corporate template comes back plainer (see the limits). Task 9 adds shapes, a master and transitions, and presents the deck.

**Claude.**

> Turn these notes into a 5-slide deck at /home/me/talks/build.pptx, teal theme, with a PDF copy: title "Moving to the new build system", why change (14 min builds, nobody understands the scripts, CI cost doubled), old vs new side by side, a 3-step plan, and a questions slide. Put "start with the CI bill" in the speaker notes.

Claude calls:

1. `office_deck_new` with an `outline`. Writing the whole deck as one outline is the fastest way.
2. `office_deck_preview` with `all=true`, which returns every slide as an image so Claude can look for crowded slides.
3. `office_deck_info`, which also warns about text that doesn't fit.
4. `office_deck_edit_slide` to fix anything it doesn't like.
5. `office_deck_save` to `.pptx` and `office_deck_export` to `.pdf`.

## Task 5: merge and split PDFs

You want to combine the report and part of the slide deck into one handout, split it up again in different ways, reorder and rotate pages, read the text, and turn the two chart images into a PDF.

**CLI.** The PDF commands work on files directly; there is nothing to open.

```sh
$ wetwaresuite office.pdf_info path=talk.pdf
talk.pdf: 5 page(s), PDF 1.4
title: Moving to the new build system
author: Platform team
creator: wetwaresuite office
producer: wetwaresuite
  pages 1-5: 960 x 540 pt (16:9)
has extractable text
5
pages: 5
version: 1.4
encrypted: false
width: 960
height: 540

$ wetwaresuite office.pdf_merge inputs="q1-budget.pdf; talk.pdf[2-4]" output=pack.pdf
merged q1-budget.pdf + talk.pdf[2-4] -> pack.pdf (5 pages)
pack.pdf
output: pack.pdf
pages: 5

$ wetwaresuite office.pdf_split path=pack.pdf output=parts/pack-{n}.pdf
split pack.pdf into 5 files:
  parts/pack-1.pdf: pages 1
  parts/pack-2.pdf: pages 2
  parts/pack-3.pdf: pages 3
  parts/pack-4.pdf: pages 4
  parts/pack-5.pdf: pages 5
parts/pack-1.pdf
files: 5
first: parts/pack-1.pdf

$ wetwaresuite office.pdf_split path=pack.pdf ranges="1-2; 3-" output=parts/section.pdf
split pack.pdf into 2 files:
  parts/section-1.pdf: pages 1-2
  parts/section-2.pdf: pages 3-5
...

$ wetwaresuite office.pdf_extract path=talk.pdf pages="5,1" output=bookends.pdf
extracted pages 5,1 -> bookends.pdf (2 pages)
...

$ wetwaresuite office.pdf_rotate path=bookends.pdf pages=2 angle=90 output=rotated.pdf
rotated pages 2 by 90 degrees -> rotated.pdf (2 pages)
...

$ wetwaresuite office.pdf_delete_pages path=pack.pdf pages=last output=pack-short.pdf
deleted pages 5 -> pack-short.pdf (4 pages)
...

$ wetwaresuite office.pdf_text path=pack.pdf pages=3
--- page 3 ---
Why change

• Clean builds take 14 minutes
• Nobody can explain the old scripts
– three generations of Makefiles
• CI cost doubled this year

2
pages: 1

$ wetwaresuite office.pdf_from_images images="budget-chart.png, budget-pie.png" output=charts.pdf page=a4
made charts.pdf from 2 image(s)
...

$ wetwaresuite office.pdf_render path=lo/talk-built.pdf page=4 dpi=50 output=lo-page4.png
page 4 of 5 at 50 dpi (667x375); wrote lo-page4.png
output: lo-page4.png
attachment: page4.png (13574 bytes)

$ wetwaresuite office.convert input=pack.pdf output=pack.txt
converted pack.pdf to pack.txt (built-in, 5 pages of text, 1 KB)
...
```

Page selections are the same everywhere: `1-3,7`, `10-` (to the end), `last`, `5-1` (reversed), and repeats are allowed in `pdf_extract` and `pdf_merge`. In `pdf_merge`, each input can carry a selection in brackets (`report.pdf[2-5]`). The inputs don't have to come from the suite: PDFs from LibreOffice, browsers and scanners merge the same way (the `lo/talk-built.pdf` above was made by LibreOffice). Splitting with `{n}` in the name numbers the parts. Without `{n}`, the number goes before the extension (`section-1.pdf`). Leave `output` out of `pdf_rotate` and `pdf_delete_pages` to change the file in place.

**Batch.** PDF commands are not undoable (they write files), so a batch script is just a list of them:

```text
# handout.txt - the report plus three slides, then two pages per file for printing
office.deck_export file=talk.md path=talk.pdf
office.doc_export file=q1-budget.md path=q1-budget.pdf
office.pdf_merge inputs="q1-budget.pdf; talk.pdf[2-4]" output=handout.pdf
office.pdf_info path=handout.pdf
office.pdf_split path=handout.pdf every=2 output=print/sheet-{n}.pdf
```

```sh
$ wetwaresuite batch handout.txt
exported talk.pdf (5 slides)
...
exported q1-budget.pdf (2 pages)
...
merged q1-budget.pdf + talk.pdf[2-4] -> handout.pdf (5 pages)
...
handout.pdf: 5 page(s), PDF 1.7
producer: wetwaresuite
  pages 1-2: 595.3 x 841.9 pt (A4)
  pages 3-5: 960 x 540 pt (16:9)
has extractable text
...
split handout.pdf into 3 files:
  print/sheet-1.pdf: pages 1-2
  print/sheet-2.pdf: pages 3-4
  print/sheet-3.pdf: pages 5
...
```

The first two lines show that the one-shot exports work inside scripts as well.

**Claude.**

> In /home/me/handouts, merge report.pdf and pages 2-4 of slides.pdf into handout.pdf, then make a separate file of just the last page turned sideways. Show me page 3 of the handout.

Claude calls `office_pdf_info` on each input to check the page counts, then `office_pdf_merge`, then `office_pdf_extract` with `pages=last` and `office_pdf_rotate` with `angle=90`. To look at page 3 it calls `office_pdf_render` (when pdftoppm is installed) and gets the page as an image; without pdftoppm, `office_pdf_text` reads the page instead. When a PDF has no text (`pdf_info` says so), it is a scan, and Claude will say that the text can't be extracted.

## Task 6: all of it by asking Claude

Connect the server once, with just the office tools:

```sh
claude mcp add wetwaresuite-office -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools office
```

(or add `"args": ["mcp", "--tools", "office"]` in Claude Desktop's config, see [mcp.md](mcp.md)). Claude then sees 119 `office_*` tools plus `undo`, `redo`, `history` and `documents`. A whole afternoon's work can be one request:

> In /home/me/q1:
> 1. Make budget.xlsx with rent 1200/1200/1200, groceries 420/385/450, transport 95/110/80 and fun 150/90/200 for Jan-Mar, with totals across and down in currency format and a bar chart of spending by month saved as budget-chart.png.
> 2. Clean up orders.csv: trimmed upper-case customer ids, customer names joined from customers.csv (mark unknown ids), saved as orders-clean.csv. Tell me which ids weren't found.
> 3. Write a report from q1-notes.md and save it as report.pdf, with the chart in it.
> 4. Make a 5-slide deck about the quarter in the teal theme, as talk.pptx and talk.pdf.
> 5. Merge report.pdf and talk.pdf into pack.pdf, and split pack.pdf into the report part and the slides part.
> 6. Convert talk.pptx to talk.odp for the people on LibreOffice.

This sequence was run against the server over JSON-RPC (the same messages Claude sends), using the files from tasks 1 to 5. Here are the tool calls, each with the first line of its answer:

```text
office_sheet_new        {"name": "Budget"}                                  created sheet1: 1 sheet: Budget (empty)
office_sheet_set        {"doc": "sheet1", "range": "A1", "csv": "Category,Jan,Feb,Mar\nRent,1200,..."}
                                                                            set A1:D5 (20 cells); A1 = Category
office_sheet_set        {"range": "E2:E5", "value": "=SUM(B2:D2)"}          set E2:E5 (4 cells); E2 = 3600
office_sheet_set        {"range": "B6:E6", "value": "=SUM(B2:B5)"}          set B6:E6 (4 cells); B6 = 1865
office_sheet_format     {"range": "B2:E6", "format": "currency"}            formatted B2:E6: format $#,##0.00
office_sheet_chart      {"range": "A1:D5", "title": "Spending by month", "path": ".../budget-chart.png"}
                                                                            bar chart of A1:D5: 3 series x 4 categories ... [1 image]
office_sheet_save       {"path": ".../budget.xlsx"}                         saved .../budget.xlsx
office_sheet_open       {"path": ".../orders.csv"}                          opened sheet2: 1 sheet: orders A1:D9
office_sheet_add        {"doc": "sheet2", "name": "Customers", "csv_file": ".../customers.csv"}
                                                                            added sheet Customers at position 2
office_sheet_insert_cols {"doc": "sheet2", "sheet": "orders", "at": "C"}    inserted column C in orders; formulas adjusted
office_sheet_set        {"range": "orders!C2:C9", "value": "=UPPER(TRIM(B2))"}
                                                                            set orders!C2:C9 (8 cells); C2 = C-002
office_sheet_set        {"range": "orders!F2:F9", "value": "=IFERROR(VLOOKUP(C2,Customers!A:C,2,FALSE),\"unknown\")"}
                                                                            set orders!F2:F9 (8 cells); F2 = Birch & Co
office_sheet_find       {"doc": "sheet2", "query": "unknown"}               1 cell(s) match "unknown": orders!F7 ...
office_sheet_save       {"path": ".../orders-clean.csv", "sheet": "orders"} saved .../orders-clean.csv (sheet orders only; ...)
office_doc_new          {"markdown_file": ".../q1-notes.md"}                created doc1: "Q1 household budget", 11 blocks
office_doc_preview      {"doc": "doc1", "page": 1}                          page 1 of 2 [1 image]
office_doc_export       {"path": ".../report.pdf"}                          exported .../report.pdf (2 pages)
office_deck_new         {"outline": "# Moving to ...\n...", "theme": "teal"} created deck1: "...", 5 slides, theme teal
office_deck_info        {"doc": "deck1"}                                    deck1: "...", 5 slide(s), theme teal, 16:9
office_deck_preview     {"doc": "deck1", "all": true}                       previews of 5 slides [5 images]
office_deck_save        {"path": ".../talk.pptx"}                           saved .../talk.pptx (5 slides)
office_deck_export      {"path": ".../talk.pdf"}                            exported .../talk.pdf (5 slides)
office_pdf_merge        {"inputs": ".../report.pdf;.../talk.pdf", "output": ".../pack.pdf"}
                                                                            merged report.pdf + talk.pdf -> .../pack.pdf (7 pages)
office_pdf_split        {"path": ".../pack.pdf", "ranges": "1-2;3-"}        split .../pack.pdf into 2 files: pack-1.pdf: pages 1-2, pack-2.pdf: pages 3-7
office_convert          {"input": ".../talk.pptx", "output": ".../talk.odp"}
                                                                            converted ... (built-in + LibreOffice, 5 slides, 36 KB)
```

(Shortened: `...` stands for the folder, and `doc` is left out where only one document of that kind was open.) Things that make this go well:

- **Give absolute paths**, or say which folder to work in. The server runs wherever Claude started it.
- **Let Claude look.** The preview commands and `preview=true` return images, and the chart command always returns its PNG. Claude checks its own slides and pages and fixes crowded ones before saving. Ask "show me slide 3" or "what does page 2 look like" to see it yourself.
- **Ask questions without changing anything.** "What did we spend on groceries in March?" becomes `office_sheet_calc` or `office_sheet_get`. "How long is this report?" becomes `office_doc_word_count`. "What's in this PDF?" becomes `office_pdf_info` plus `office_pdf_text`. These are read-only.
- **Undo works.** "Undo that sort" calls `undo`, and `history` lists the steps. Saves and PDF commands write files, so they can't be undone; the file on disk stays.
- **`documents` lists what's open**, with ids, so a long conversation can go back to `sheet2` or `deck1`.

## Task 7: a sales tracker the Excel way

You have a CSV export of sales with day-first dates. You want it to look like a real workbook: a merged title, a styled header, frozen headings, currency, an id column filled in as a series, a named range, highlighted big sales with data bars, a dropdown that only allows the four regions, and a pivot table of sales by region and product with a chart. The result goes out as `.xlsx` for Excel, `.ods` for LibreOffice and a PDF.

`sales.csv`:

```text
Date,Rep,Region,Product,Amount
03/01/2026,Ann,North,Pens,1200
05/01/2026,Bob,South,Ink,340
09/01/2026,Cid,North,Ink,560
14/02/2026,Ann,East,Pens,980
20/02/2026,Dee,South,Pens,1500
02/03/2026,Bob,East,Ink,210
11/03/2026,Cid,North,Pens,760
28/03/2026,Dee,South,Ink,1130
```

**CLI.** Every step is one command. Two on their own are handy from a shell:

```sh
# what's in an .xlsx someone sent you: rules, validation, names, charts
wetwaresuite office.sheet_cond_formats file=tracker.xlsx sheet=Sales
wetwaresuite office.sheet_validations file=tracker.xlsx sheet=Sales
wetwaresuite office.sheet_names file=tracker.xlsx
wetwaresuite office.sheet_charts file=tracker.xlsx sheet=Pivot chart=1
```

**Batch.**

```text
# tracker.txt - a sales tracker with styles, rules, validation, a chart and a pivot
office.sheet_open path=sales.csv date_order=dmy
office.sheet_rename sheet=sales name=Sales
office.sheet_format range=A:A format=date
office.sheet_get range=A1:E4

# a series down column F: one id per row
office.sheet_set range=F1 value=Ref
office.sheet_set range=F2 value=INV-001
office.sheet_fill range=F2:F9

# a title row, merged across the table
office.sheet_insert_rows at=1
office.sheet_set range=A1 value="Q1 sales"
office.sheet_merge range=A1:F1
office.sheet_style range=A1 bold=true font_size=14 fill=navy color=white
office.sheet_style range=A2:F2 bold=true fill=lightgray border=bottom border_style=medium
office.sheet_format range=E3:E10 format=currency
office.sheet_format range=A:F autofit=true
office.sheet_freeze at=A3

# named ranges, conditional formats and validation
office.sheet_name name=Amounts refers_to=Sales!$E$3:$E$10
office.sheet_set range=H2 value="Total"
office.sheet_set range=H3 value="=SUM(Amounts)"
office.sheet_cond_format range=E3:E10 op=">=" value=1000 fill=lightgreen bold=true
office.sheet_cond_format range=E3:E10 type=data_bar
office.sheet_validate range=C3:C100 type=list value="North,South,East,West"
office.sheet_set range=C5 value=Middle
office.sheet_validations

# a live pivot, a chart and the file
office.sheet_pivot rows=Region columns=Product values=Amount range=A2:E10
office.sheet_chart_add range=A1:C4 sheet=Pivot type=bar title="Sales by region"
office.sheet_get sheet=Pivot
office.sheet_save path=tracker.xlsx
office.sheet_save path=tracker.ods
office.sheet_save path=tracker.pdf
```

```sh
$ wetwaresuite batch tracker.txt --keep-going
opened sheet1: 1 sheet: sales A1:E9 - sales.csv
...
A1:E4 (4 rows x 5 cols)
	A	B	C	D	E
1	Date	Rep	Region	Product	Amount
2	2026-01-03	Ann	North	Pens	1200
3	2026-01-05	Bob	South	Ink	340
4	2026-01-09	Cid	North	Ink	560
...
series 7 cells of F2:F9 down
inserted row 1 in Sales; formulas adjusted
...
merged A1:F1
styled A1: bold, colour #ffffff, fill #1f3864, size 14pt
styled A2:F2 (6 cells): borders bottom medium, bold, fill #d9d9d9
formatted E3:E10: format $#,##0.00
formatted A:F: autofit widths
froze rows 1-2 of Sales
defined Amounts = Sales!$E$3:$E$10 (8x1 cells)
...
set H3; H3 = 6680
...
rule 1 on E3:E10: value >= 1000 -> bold, fill #c6efce; 3 cell(s) match now
...
rule 2 on E3:E10: data bars #638ec6
...
validation on C3:C100: list of North,South,East,West
line 29: C5 must be one of: North, South, East, West
Sales: 1 rule(s)
  1. C3:C100: list of North,South,East,West
...
Sum of Amount by Region and Product: 3 row(s) x 2 column(s) in the new sheet Pivot (A1:D5)
chart 1: bar chart of A1:C4 "Sales by region" at E1, 480x288px
...
Pivot!A1:D5 (5 rows x 4 cols)
	A	B	C	D
1	Region / Product	Ink	Pens	Total
2	East	$210.00	$980.00	$1,190.00
3	North	$560.00	$1,960.00	$2,520.00
4	South	$1,470.00	$1,500.00	$2,970.00
5	Total	$2,240.00	$4,440.00	$6,680.00
saved tracker.xlsx
...
saved tracker.ods
...
saved tracker.pdf
```

(Shortened. `--keep-going` lets the script carry on after the line that validation rejected on purpose.) Some things to notice:

- **Fill continues series** the way Excel's fill handle does: `INV-001` becomes `INV-002`, `INV-003`... Two numbers (`source=2`) continue their step, `Jan` becomes `Feb`, `Mon` becomes `Tue`, and dates a month apart keep going by months. `step=` sets the step, `unit=weekday` skips weekends, `series=growth` multiplies and `series=copy` just repeats.
- **Inserting a row moves everything with it**: the merge, the rules, the validation and the named range all point at the right cells afterwards.
- **Validation is enforced.** A `stop` rule (the default) rejects a bad value, and the cell keeps what it had. With `error_style=warning` the value goes in and the answer carries a warning. `office.sheet_validations` lists the cells that already break a rule, and `office.sheet_get` on one cell says which rule covers it.
- **Conditional formats and data bars** are saved as real Excel and LibreOffice rules, so they keep working when the numbers change there. Later rules win where two set the same thing.
- **The pivot is made of formulas** (`SUMIFS`, `COUNTIFS`, ...), so it updates when the sales change. Use `static=true` for plain numbers, `function=count|average|min|max` for other summaries, and `to=H1` to put it next to the data.
- **`.xlsx` and `.ods` keep all of it**: styles, the merge, frozen panes, number formats, rules, validation dropdowns, the named range and the chart, which is a real chart Excel and LibreOffice can edit. The PDF draws the same styles, rules and chart.

More things the same commands do:

```text
office.sheet_select sheet=Sales
office.sheet_copy from=A2:F10 to=Pivot!A10 what=values         # paste values only
office.sheet_copy from=A2:F2 to=L2 transpose=true              # rows become columns
office.sheet_move from=H2:H3 to=J2                             # cut and paste; =SUM(Amounts) keeps working
office.sheet_autofilter range=A2:F10 column=Region criteria=North   # hide the other rows
office.sheet_autofilter clear=true
office.sheet_hide cols=F
office.sheet_style range=E3:E10 like=E3                        # format painter
office.sheet_cond_format range=B3:B10 type=duplicate fill=#ffc7ce
office.sheet_cond_format range=E3:E10 type=color_scale min_color=white max_color=green
office.sheet_import path=more-sales.csv at=Sales!A11 date_order=dmy
```

```sh
$ wetwaresuite office.sheet_cond_formats file=tracker.xlsx sheet=Sales
Sales: 2 rule(s)
  1. E3:E10: value >= 1000 -> bold, fill #c6efce (3 match)
  2. E3:E10: data bars #638ec6
```

**Claude.**

> Open ~/sales/sales.csv (the dates are day first). Give it a merged navy title "Q1 sales", a grey bold header, currency amounts and frozen headings. Highlight sales of 1000 or more in green, add data bars, and make Region a dropdown of North/South/East/West. Then a pivot of amount by region and product with a bar chart, saved as tracker.xlsx.

Claude calls `office_sheet_open` with `date_order=dmy`, then `office_sheet_insert_rows`, `office_sheet_merge`, `office_sheet_style`, `office_sheet_format`, `office_sheet_freeze`, two `office_sheet_cond_format`, `office_sheet_validate`, `office_sheet_pivot`, `office_sheet_chart_add` and `office_sheet_save`. It checks the result with `office_sheet_preview`, which draws the styles, rules and chart.

**In the window.** Open `tracker.xlsx` in `wetwaresuite-office`. The grid shows the fills, font colours, bold, underline, alignment, merged cells, and the conditional formats and data bars, and they update as you type. Press **Ctrl+K** and type `style`, `merge`, `freeze`, `conditional`, `validate`, `pivot` or `chart` to get each command's form. It works on the open sheet, and Ctrl+Z undoes it like a typed cell.

## Task 8: a report the Word way

You are writing a short field report and want it to look and behave like a Word document: a title, a table of contents, styled headings, a shaded table with a merged cell, a footnote, a header and a footer with page numbers. Then a colleague reviews it with a comment and tracked changes, and it goes out as `.docx` for Word, `.odt` for LibreOffice and a PDF.

A document is a list of numbered blocks (`office.doc_info` lists them). Formatting commands name a block, and optionally a piece of text in it (`find=`), a list item (`item=`) or a table cell (`row=` and `col=`).

**CLI.** A `.docx` or `.odt` someone sent you can be inspected without opening it:

```sh
$ wetwaresuite office.doc_comments file=field.docx
1 comment(s):
  1. Ann on block 4 "three sites": Name them?
count: 1
$ wetwaresuite office.doc_changes file=field.docx
track changes on; 4 change(s):
  1. deleted by Ann 2026-10-22 in block 4: "three"
  2. inserted by Ann 2026-10-22 in block 4: "four"
...
$ wetwaresuite office.doc_footnotes file=field.odt
1 footnote(s):
  1. (block 4) From *1 May* to 12 June.
count: 1
$ wetwaresuite office.doc_styles file=field.docx
14 styles:
  Normal: sans 11 pt, space 0/8.25 pt, line 1.1; 3 block(s)
  Title: sans 26.4 pt bold #1f3864, space 0/12 pt, line 1.1; 1 block(s)
...
  Heading 1: sans 19.8 pt bold #7a2e8e, space 16/4 pt, line 1.1 (changed); 2 block(s)
...
  Callout: sans 12 pt italic #1f6f3f, indent 24, space 0/8.25 pt, line 1.1 (yours); 1 block(s)
```

**Batch.**

```text
# report.txt - a field report the Word way: styles, a table, notes, review
office.doc_new title="Field report" author="Sam Rivera"
office.doc_add text="Field report"
office.doc_format block=1 style=Title
office.doc_add kind=heading text=Summary
office.doc_add text="The pilot ran for six weeks across three sites and met every target."
office.doc_add kind=heading text=Results
office.doc_add kind=table text="Site | Visits | Change\nNorth | 1,200 | +12%\nSouth | 950 | +4%\nTotal | 2,150 | +8%"
office.doc_add kind=numbered numbering=lowerRoman text="Keep the north schedule\nHire for south"
office.doc_add kind=heading level=2 text=Notes
office.doc_add text="Costs stayed under budget."

# character and paragraph formatting
office.doc_format block=3 find="every target" bold=true color=#c00000 highlight=yellow
office.doc_format block=3 align=justify first_line=18 space_after=12

# styles: one of your own, and a change to a built-in one
office.doc_style_define name=Callout italic=true color=#1f6f3f indent=24 size=12
office.doc_format block=8 style=Callout
office.doc_style_define name="Heading 1" color=#7a2e8e

# the table: shading, alignment, a new row, a merged cell, widths
office.doc_table_cell block=5 row=4 col=1 to_col=3 fill=#dde7f3
office.doc_table_cell block=5 row=2 col=2 to_row=4 to_col=3 align=right
office.doc_table_insert block=5 row=3 text="East | 400 | +9%"
office.doc_table_merge block=5 row=5 col=2 cols=2
office.doc_table_format block=5 widths=2,1,1 banded=true

# page furniture
office.doc_header_footer header="{title} | | {date}" footer="Page {page} of {pages}"
office.doc_toc levels=2
office.doc_footnote block=4 after="six weeks" text="From *1 May* to 12 June."

# review: a comment, then edits recorded as tracked changes
office.doc_comment block=4 find="three sites" text="Name them?" author=Ann
office.doc_track on=true author=Ann
office.doc_edit block=4 text="The pilot ran for six weeks across four sites and met every target."
office.doc_find_replace find=budget replace=plan
office.doc_changes

office.doc_save path=field.docx
office.doc_export path=field.odt
office.doc_export path=field.pdf
```

```sh
$ wetwaresuite batch report.txt
created doc1: "Field report", 0 blocks
...
formatted block 1: style Title
...
formatted block 3 (1 match(es) of "every target"): bold, color #c00000, highlight #ffff00
formatted block 3: aligned justify, first line 18, space after 12
created style Callout: size 12 pt, color #1f6f3f, italic, indent 24 (sans 12 pt italic #1f6f3f, indent 24, space 0/8.25 pt, line 1.1; used by 0 block(s))
...
formatted block 8: style Callout
changed style Heading1: color #7a2e8e (sans 19.8 pt bold #7a2e8e, space 16/4 pt, line 1.1; used by 2 block(s))
...
table 5 rows 4-4, columns 1-3: fill #dde7f3
table 5 rows 2-4, columns 2-3: align right
inserted 1 row(s) below row 3 in table 5 (now 5x3)
merged 1x2 cells at row 5, column 2 in table 5
table 5: banded rows, widths 2,1,1
header: {title} | | {date}
footer: Page {page} of {pages}
table of contents at block 2: 3 heading(s), levels 1-2
...
added footnote 1 in block 4 (1 in all)
...
comment 1 by Ann on block 4: Name them?
...
track changes on, markup shown, author Ann; 0 change(s) pending
block 4. paragraph: The pilot ran for six weeks across threefour sites and met every target.
replaced 1 match(es) of "budget" with "plan" in block(s) 9 (tracked changes 3 and 4)
...
track changes on; 4 change(s):
  1. deleted by Ann 2026-10-22 in block 4: "three"
  2. inserted by Ann 2026-10-22 in block 4: "four"
  3. deleted by Ann 2026-10-22 in block 9: "budget"
  4. inserted by Ann 2026-10-22 in block 9: "plan"
count: 4
saved field.docx
...
exported field.odt
...
exported field.pdf (1 pages)
```

(Shortened.) The table of contents went in as block 2, so the blocks after it moved down one: `office.doc_info` shows the numbers at any time. Some things to notice:

- **The PDF and the previews look like the Word page.** The title, the purple headings, the contents with dot leaders and page numbers, the header with the title and date, "Page 1 of 1" in the footer, the footnote at the foot of the page, and the tracked changes (`three` struck through, `four` underlined) with the commented words highlighted. `office.doc_track show_markup=false` draws the final text instead.
- **Styles work like Word's.** The built-in styles are Normal, Title, Subtitle, Heading 1-6, Quote, Code, Caption and List Paragraph. `office.doc_style_define` changes one (every heading follows) or makes your own, `based_on=` another. `style=Heading 2` on a paragraph turns it into a heading, so it shows in the table of contents. `office.doc_styles` lists them all with how they look.
- **Direct formatting sits on top of the style**: bold, italic, underline, strike, colour, highlight, size, font, superscript and subscript, links, alignment, indents (`first_line=-18` is a hanging indent), spacing, line spacing, keep with next and page break before. `clear=true` takes it off again.
- **The table of contents updates itself**: it is drawn from the headings each time, with the pages they land on. Word and LibreOffice see a real table of contents field.
- **Tracked changes.** While `office.doc_track on=true`, `doc_add`, `doc_edit`, `doc_delete`, `doc_find_replace`, `doc_table_cell text=` and `doc_footnote` are recorded as insertions and deletions by the `author=` you gave (default: your login name). `office.doc_info` and `doc_get_text` show both the old and the new text until you decide. `office.doc_accept` and `office.doc_reject` take one change (`id=`) or all of them.
- **What the files keep.** `.docx` keeps all of it: styles, formats, the merged and shaded cells, column widths, the list numbering, header and footer with page fields, the footnote, the comment, the tracked changes and the table of contents. `.odt` keeps the same, except that tracked changes are saved accepted. Both open again here with everything in place.

More things the same commands do:

```text
office.doc_format block=4 find=pilot link=https://example.com/pilot
office.doc_format block=4 to=9 clear=true                         # a block range, back to the styles
office.doc_format block=7 item=2 find=south position=superscript
office.doc_list block=7 numbering=upperLetter start=3              # C. D. ...
office.doc_list block=7 item=2 level=2                             # indent one item
office.doc_table_merge block=6 row=5 col=2 unmerge=true
office.doc_table_delete block=6 col=3
office.doc_table_cell block=6 row=1 col=1 text="**Place**" valign=middle
office.doc_header_footer different_first=true first_footer=""     # nothing on the title page
office.doc_setup margin_left=30 margin_right=20 landscape=true
office.doc_comment_resolve id=1
office.doc_comment_delete all=true
office.doc_footnote number=1 text="From 1 May to 14 June."         # change a note
office.doc_accept id=1
office.doc_reject
```

**Claude.**

> Open ~/reports/field.docx. Make the title use the Title style, add a table of contents for two heading levels, shade the Total row light blue and right-align the numbers, put "Page n of N" in the footer, and add a footnote after "six weeks" saying it ran from 1 May to 12 June. Then turn on track changes as Ann, change "three sites" to "four sites", and save it as field.docx and field.pdf.

Claude calls `office_doc_open`, `office_doc_info` to learn the block numbers, then `office_doc_format` with `style=Title`, `office_doc_toc`, two `office_doc_table_cell`, `office_doc_header_footer`, `office_doc_footnote`, `office_doc_track`, `office_doc_find_replace` and `office_doc_save` twice. It checks the page with `office_doc_preview`. Asked "what did Ann change?", it calls `office_doc_changes`; "accept them all" is `office_doc_accept`.

**In the window.** Open `field.docx` in `wetwaresuite-office`. The page shows as it prints, with the header, footer, footnotes, tracked changes and highlighted comments. Press **Ctrl+K** and type `format`, `style`, `table`, `footnote`, `comment`, `track`, `accept` or `contents` to get each command's form; **Document** is already set to the open file, and Ctrl+Z undoes each step.

## Task 9: a launch talk the PowerPoint way

You are giving a launch talk and want it to look like a designed PowerPoint deck, not only bullets. You want a picture with a caption, a row of chevrons with an arrow and a callout, a logo, a footer and slide numbers on every slide, and transitions. Then you want to present it, with your notes on a second screen, and send the `.pptx` and a PDF of the notes afterwards.

Slides still start from the layouts of Task 4. On top of the layout, each slide (and the master) holds **objects**: text boxes, shapes, lines and pictures, placed in points from the top-left corner. A 16:9 slide is 960 x 540 points (4:3 is 720 x 540). `position=` (`center`, `top`, `bottom_right`, ...) places an object for you, 48 points from the edges. The first object is at the back. `office.deck_objects` lists a slide's objects with their numbers, and `name=` gives one a name to use instead of its number.

**CLI.** A `.pptx` from PowerPoint, Google Slides or LibreOffice can be looked at, and presented, without opening it:

```sh
$ wetwaresuite office.deck_objects file=launch.pptx slide=3
slide 3: 5 object(s)
  1. chevron at 60,200 size 260x110, fill accent, text "Build" 18 pt #ffffff center
  2. chevron at 340,200 size 260x110, fill #f39c12, text "Test" 18 pt #222222 center
  3. chevron "ship" at 620,200 size 260x110, fill #2e7d32, text "Ship" 18 pt bold #ffffff center
  4. line from 100,380 to 840,380, accent 3 pt, arrow
  5. callout at 660,400 size 200x60, fill #fff3c4, outline #c08a00 1.5 pt, text "On track!" 18 pt #222222 center
count: 5
$ wetwaresuite office.deck_notes file=launch.pptx
2 of 4 slide(s) have notes:
1. Launch plan: (no notes)
2. Where we are: Mention the beta survey.
3. Three steps to launch: Six weeks in all.
4. Questions?: (no notes)
count: 2
$ wetwaresuite office.deck_present file=launch.pptx
slide show of 4 slide(s) in launch-show.html: open it in a browser. Space or arrow keys move on, F full screen, P presenter window, N notes, ? help.
...
```

**Batch.**

```text
# launch.txt - a launch talk the PowerPoint way: shapes, a master, transitions, a show
office.deck_new title="Launch plan" author="Sam Rivera" theme=teal
office.deck_add_slide layout=title title="Launch plan" subtitle="Platform team, Q4 2026"
office.deck_add_slide title="Where we are" bullets="Beta with 40 teams\nCrash rate down 60%\nDocs rewritten" notes="Mention the beta survey."
office.deck_add_slide layout=title_only title="Three steps to launch" notes="Six weeks in all."
office.deck_add_slide layout=section title="Questions?" subtitle="launch@example.com"

# pictures and text boxes go anywhere (points; the slide is 960 x 540)
office.deck_image slide=2 path=budget-chart.png x=560 y=140 w=340
office.deck_text_box slide=2 text="Survey, October" x=560 y=400 w=340 size=14 italic=true align=center

# shapes with text, an arrow and a callout
office.deck_shape slide=3 shape=chevron x=60 y=200 w=260 h=110 text=Build
office.deck_shape slide=3 shape=chevron x=340 y=200 w=260 h=110 text=Test fill=#f39c12
office.deck_shape slide=3 shape=chevron x=620 y=200 w=260 h=110 text="**Ship**" fill=#2e7d32 name=ship
office.deck_line slide=3 x=100 y=380 x2=840 y2=380 arrow=true color=accent line_width=3
office.deck_shape slide=3 shape=callout x=660 y=400 w=200 h=60 text="On track!" fill=#fff3c4 line=#c08a00
office.deck_objects slide=3

# the master: a logo, a footer and slide numbers on every content slide
office.deck_image master=true path=logo.png x=840 y=20 w=80
office.deck_master footer="Launch plan | Confidential"

# transitions and timings
office.deck_transition all=true type=fade
office.deck_transition slide=3 type=push direction=left duration=1

office.deck_info
office.deck_save path=launch.pptx
office.deck_export path=launch.pdf
office.deck_export path=launch-notes.pdf notes=true
office.deck_present path=launch-show.html
```

```sh
$ wetwaresuite batch launch.txt
created deck1: "Launch plan", 0 slides, theme teal
...
added to slide 2: 1. image at 560,140 size 340x213, picture 800x500
...
added to slide 2: 2. text box at 560,400 size 340x24 (auto height), text "Survey, October" 14 pt italic center
...
added to slide 3: 1. chevron at 60,200 size 260x110, fill accent, text "Build" 18 pt center
...
added to slide 3: 4. line from 100,380 to 840,380, accent 3 pt, arrow
...
added to the master: 1. image at 840,20 size 80x40, picture 120x60
...
master: slide numbers on, footer "Launch plan | Confidential", 1 object(s), not on title and section slides
4 slides: fade 0.75 s, advances on click
slide 3: push left 1 s, advances on click
deck1: "Launch plan", 4 slide(s), theme teal, 16:9
  master: slide numbers, footer "Launch plan | Confidential", 1 object(s)
  1. title: "Launch plan" / Platform team, Q4 2026, fade transition
  2. content: "Where we are", 3 bullet(s), notes, 2 object(s), fade transition
  3. title_only: "Three steps to launch", notes, 5 object(s), push transition
  4. section: "Questions?" / launch@example.com, fade transition
...
saved launch.pptx (4 slides)
...
exported launch.pdf (4 slides)
...
exported notes pages to launch-notes.pdf (4 pages)
...
slide show of 4 slide(s) in launch-show.html: open it in a browser. Space or arrow keys move on, F full screen, P presenter window, N notes, ? help.
...
```

(Shortened.) Some things to notice:

- **Shapes.** `office.deck_shape` draws `rect`, `rounded`, `ellipse`, `triangle`, `right_triangle`, `diamond`, `pentagon`, `hexagon`, `octagon`, `star`, `arrow_right`, `arrow_left`, `arrow_up`, `arrow_down`, `chevron`, `parallelogram`, `trapezoid`, `plus` and `callout`. Each one has a fill, an outline (`line=`, `line_width=`) and optional text, which is centred and shrinks to fit. In PowerPoint they are real shapes of the same kind, so you can still drag, recolour and type into them there.
- **Colours** are `#rrggbb`, a name such as `red`, `none`, or a theme colour: `accent`, `text` or `background`. Theme colours follow `office.deck_theme`, in the suite and in PowerPoint. Text with no `color=` is the theme's text colour, or white or dark when that reads better on the fill.
- **Text boxes** take inline Markdown and one paragraph per line, or bullets with `bullets=true`. Without `h=` a text box grows with its text, as in PowerPoint.
- **The master** holds what every slide shares: objects added with `master=true` (a logo here), the footer and the slide numbers. They show on every slide except title and section slides; `office.deck_master on_title=true` shows them there too.
- **Transitions** are `fade`, `push`, `wipe`, `cover` and `zoom`, with a direction and a duration. `advance_after=` moves to the next slide by itself after that many seconds. All of it is kept in `.pptx`, and the slide show plays it.
- **Presenting.** `office.deck_present` writes one `.html` file that any browser can show full screen (F). Space, the arrow keys or a click move through the slides, and typing a number and Enter jumps to that slide. B or W blanks the screen, and N shows your notes under the slide. P opens a presenter window for your second screen with the current and next slide, your notes and a timer. Hidden slides are left out. `loop=true` with `advance_after=` timings makes a kiosk show.
- **Handouts.** `office.deck_export notes=true` writes notes pages, with each slide above its speaker notes, the way PowerPoint prints them.
- **What `.pptx` keeps.** Everything above: layouts, objects with their geometry, colours and text, pictures, the master's objects, footer and slide numbers, theme colours and font, notes, transitions and timings, and hidden slides. Decks from other programs open with their pictures, text boxes, shapes, lines and grouped shapes kept where they were, and their master's shapes become master objects.

More things the same commands do:

```text
office.deck_object_edit slide=3 object=ship text="**Ship** in December" fill=#1b5e20
office.deck_object_edit slide=3 object=5 position=bottom_right order=back
office.deck_shape slide=4 shape=star x=420 y=60 w=120 h=120 fill=#ffd54f line=none
office.deck_text_box slide=2 text="- one\n- two" bullets=true x=48 y=380 w=400 fill=#eef6f6 line=accent
office.deck_object_delete slide=2 object=2
office.deck_duplicate_slide slide=3
office.deck_edit_slide slide=4 hidden=true background=#102030
office.deck_theme background=#fdf6e3 text_color=#073642
office.deck_master slide_numbers=false on_title=true
office.deck_transition slide=2 type=wipe direction=up advance_after=8
office.deck_transition all=true type=none
office.deck_present path=kiosk.html loop=true
office.deck_object_delete master=true all=true
```

**Claude.**

> Open ~/talks/launch.pptx. On slide 3 draw three chevrons for Build, Test and Ship (Ship in green) with an arrow under them and an "On track!" callout. Put logo.png in the top right corner of every slide, add the footer "Launch plan | Confidential", make every slide fade in, then give me a slide show I can present from the browser and a PDF of my notes.

Claude calls `office_deck_open`, `office_deck_info`, three `office_deck_shape`, `office_deck_line` and another `office_deck_shape` for the callout. It looks at the slide with `office_deck_preview` and moves anything that overlaps with `office_deck_object_edit`. Then it calls `office_deck_image` with `master=true`, `office_deck_master`, `office_deck_transition` with `all=true`, `office_deck_present`, `office_deck_export` with `notes=true` and `office_deck_save`. Asked "what do my notes say?", it calls `office_deck_notes`.

**In the window.** Open `launch.pptx` in `wetwaresuite-office`. The window shows the deck as a slide sorter: every slide drawn as it prints, numbered, with its transition, and hidden slides faded. Press **Ctrl+K** and type `shape`, `text box`, `picture`, `object`, `master`, `transition`, `duplicate` or `present` to get each command's form; Ctrl+Z undoes each step.

## Formulas

Formulas start with `=` (optional in `office.sheet_calc`). They are recalculated whenever a cell changes, in dependency order, and circular references give `#CYCLE!`. The engine works like Excel 365: a formula that returns several values **spills** into the cells below and to the right (`=SORT(A2:A20)`, `=UNIQUE(B:B)`, `=SEQUENCE(5)`), `A2#` refers to the whole spill, and a spill that would overwrite something gives `#SPILL!`. Spills are saved as dynamic arrays in `.xlsx`, so Excel shows them the same way.

| | |
| --- | --- |
| References | `B2`, `$B$2`, `B2:D13`, whole columns `C:C`, whole rows `2:5`, other sheets `Sheet2!A1` and `'My sheet'!A1:B5`, spills `E2#`, named ranges (`office.sheet_name`) |
| Operators | `+ - * / ^`, `&` (join text), `= <> < > <= >=`, `%` (`50%` is 0.5), unary minus, `:` ranges; arrays such as `{1,2;3,4}` |
| Values | numbers, `"text"`, `TRUE`/`FALSE`; dates are day numbers like Excel's (`2026-03-01` typed into a cell becomes a date) |
| Errors | `#DIV/0!`, `#N/A`, `#NAME?` (unknown function or name), `#REF!` (deleted cells), `#VALUE!`, `#NUM!`, `#NULL!`, `#SPILL!`, `#CALC!`, `#CYCLE!` |
| Math | `SUM`, `SUMPRODUCT`, `SUMSQ`, `PRODUCT`, `SUBTOTAL`, `AGGREGATE`, `ROUND`, `ROUNDUP`, `ROUNDDOWN`, `MROUND`, `CEILING`, `CEILING.MATH`, `FLOOR`, `FLOOR.MATH`, `INT`, `TRUNC`, `MOD`, `QUOTIENT`, `ABS`, `SIGN`, `SQRT`, `POWER`, `EXP`, `LN`, `LOG`, `LOG10`, `PI`, `RAND`, `RANDBETWEEN`, `GCD`, `LCM`, `FACT`, `COMBIN`, `PERMUT`, `EVEN`, `ODD`, trigonometry (`SIN`, `COS`, `TAN`, `ASIN`, `ACOS`, `ATAN`, `ATAN2`, `SINH`, `COSH`, `TANH`, `DEGREES`, `RADIANS`), `ROMAN`, `ARABIC`, `BASE`, `DECIMAL`, `DEC2BIN`, `DEC2HEX`, `HEX2DEC`, ... |
| Statistics | `AVERAGE`, `AVERAGEA`, `MEDIAN`, `MODE`, `MIN`, `MAX`, `COUNT`, `COUNTA`, `COUNTBLANK`, `LARGE`, `SMALL`, `RANK`, `RANK.EQ`, `RANK.AVG`, `PERCENTILE`, `QUARTILE`, `PERCENTRANK`, `STDEV`, `STDEV.S`, `STDEV.P`, `VAR`, `VAR.S`, `VAR.P`, `CORREL`, `COVARIANCE.S`, `SLOPE`, `INTERCEPT`, `RSQ`, `FORECAST`, `TREND`, `FREQUENCY`, `GEOMEAN`, `HARMEAN`, `TRIMMEAN`, `NORM.DIST`, `NORM.INV`, `STANDARDIZE`, ... |
| Conditions | `SUMIF`, `SUMIFS`, `COUNTIF`, `COUNTIFS`, `AVERAGEIF`, `AVERAGEIFS`, `MAXIFS`, `MINIFS` (criteria such as `">100"`, `"<>done"`, `"a*"`, `""`) |
| Logic | `IF`, `IFS`, `SWITCH`, `IFERROR`, `IFNA`, `AND`, `OR`, `XOR`, `NOT`, `LET`, `ISBLANK`, `ISNUMBER`, `ISTEXT`, `ISERROR`, `ISNA`, `ISEVEN`, `ISODD`, `ISFORMULA`, `TYPE`, `N`, `NA` |
| Lookup | `XLOOKUP`, `XMATCH`, `VLOOKUP`, `HLOOKUP`, `LOOKUP`, `INDEX`, `MATCH`, `CHOOSE`, `OFFSET`, `INDIRECT`, `ADDRESS`, `ROW`, `ROWS`, `COLUMN`, `COLUMNS`, `TRANSPOSE`, `MMULT` |
| Dynamic arrays | `FILTER`, `SORT`, `SORTBY`, `UNIQUE`, `SEQUENCE`, `RANDARRAY`, `TAKE`, `DROP`, `CHOOSEROWS`, `CHOOSECOLS`, `HSTACK`, `VSTACK`, `TOCOL`, `TOROW`, `WRAPROWS`, `WRAPCOLS`, `EXPAND`, `TEXTSPLIT` |
| Text | `LEFT`, `RIGHT`, `MID`, `LEN`, `UPPER`, `LOWER`, `PROPER`, `TRIM`, `CLEAN`, `SUBSTITUTE`, `REPLACE`, `REPT`, `FIND`, `SEARCH`, `EXACT`, `CONCAT`, `CONCATENATE`, `TEXTJOIN`, `TEXTBEFORE`, `TEXTAFTER`, `TEXT`, `FIXED`, `DOLLAR`, `VALUE`, `NUMBERVALUE`, `CHAR`, `CODE`, `UNICHAR`, `UNICODE`, `HYPERLINK` |
| Dates | `DATE`, `TIME`, `TODAY`, `NOW`, `YEAR`, `MONTH`, `DAY`, `HOUR`, `MINUTE`, `SECOND`, `WEEKDAY`, `WEEKNUM`, `ISOWEEKNUM`, `EDATE`, `EOMONTH`, `DATEDIF`, `DAYS`, `DAYS360`, `NETWORKDAYS`, `WORKDAY` (and their `.INTL` forms), `YEARFRAC`, `DATEVALUE`, `TIMEVALUE` |
| Finance | `PMT`, `IPMT`, `PPMT`, `CUMIPMT`, `CUMPRINC`, `PV`, `FV`, `NPV`, `XNPV`, `IRR`, `XIRR`, `MIRR`, `RATE`, `NPER`, `EFFECT`, `NOMINAL`, `RRI`, `PDURATION`, `SLN`, `SYD`, `DB`, `DDB` |

About 300 functions in all. `wetwaresuite office.sheet_calc formula="=ISNUMBER(1)"` is a quick way to check whether one is there: an unknown function gives `#NAME?`. Files from Excel keep functions the engine lacks, and those cells show `#NAME?` here until the cell is rewritten.

Number formats (`office.sheet_format format=`) accept presets or Excel codes. The presets are `general`, `number` (`#,##0.00`), `integer`, `currency` (`$#,##0.00`), `euro`, `pound`, `accounting`, `percent`, `percent2`, `date` (`yyyy-mm-dd`), `datetime`, `time`, `scientific` and `text`. Codes look like `#,##0.00`, `0.0%`, `"0 kg"`, `dd/mm/yyyy` or `#,##0;[Red]-#,##0` (negative numbers in red).

## Command reference

"Read-only" means the command changes no files and no open document; `*_new` and `*_open` are read-only in that sense even though they open a document. "Undoable" commands change an open document and are one undo step each. "Not undoable" commands write files.

Two parameters are left out of the tables below because so many commands share them:

- `doc`: the id of the open document. It can be left out when only one document of that kind is open.
- `preview`: attach a PNG of the result (default false). The commands that edit a document take it.

### Spreadsheets

#### office.sheet_new
Create an empty workbook and return its id (sheet1, sheet2, ...). Optionally fill the first sheet from CSV/TSV text (cells are typed: numbers, =formulas, 15%, $1,200, 2026-03-01 dates, TRUE/FALSE). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `name` | string |  | name of the first sheet (default Sheet1) |
| `sheets` | integer |  | how many sheets to start with (default 1) |
| `csv` | string |  | initial cells as CSV or tab-separated text, one row per line |
| `csv_file` | string |  | read the CSV/TSV text from this file instead |

#### office.sheet_open
Open a spreadsheet: .xlsx/.xlsm and .ods natively with everything (formulas, styles, merges, frozen panes, conditional formats, validation, named ranges, charts), .csv/.tsv (delimiter and encoding detected, import options below); .xls and other formats through LibreOffice. Formulas are recalculated on open. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |
| `delimiter` | string |  | CSV delimiter [auto\|comma\|semicolon\|tab\|pipe\|space] (default auto) |
| `skip_rows` | integer |  | lines to skip at the top (titles above the table) |
| `decimal_comma` | boolean |  | numbers use a decimal comma: 1.234,5 = 1234.5 |
| `date_order` | string |  | how 03/04/2026 reads [auto\|dmy\|mdy] (default auto) |
| `text_columns` | string |  | columns kept as text (ids with leading zeros), e.g. A,C |
| `detect_types` | boolean |  | turn numbers, dates and TRUE/FALSE into values (false keeps every field as text) |
| `encoding` | string |  | text encoding [auto\|utf-8\|latin1] (default auto) |

#### office.sheet_save
Save the workbook. The extension picks the format: .xlsx and .ods (everything: all sheets, formulas with cached values, number formats, cell styles, merges, frozen panes, hidden rows, conditional formats, validation, named ranges, charts), .csv/.tsv (one sheet's computed values, no formulas), .pdf (printable pages with styles and charts), .html (tables); .xls and .fods through LibreOffice. Without path it saves back to where it was opened or last saved. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the document's own path) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_close
Close a workbook without saving (undo reopens it). Undoable.

#### office.sheet_info
Describe a workbook: its sheets with used ranges, cell and formula counts, errors, the active sheet and file path. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |

#### office.sheet_get
Read cells. One cell gives its value, type, formula, format and what it reads from / what reads it. A range gives a tab-separated grid with column letters and row numbers (show=formulas lists what to type to recreate each cell; show=both adds formulas next to values). Without range: the used area of the sheet. format=csv returns plain CSV. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | cell or range: B2, A1:D20, C:C, 'My sheet'!A1:B5 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `show` | string |  | what to show for each cell [values\|formulas\|both] (default values) |
| `format` | string |  | grid (row numbers + letters, tab-separated) or csv [grid\|csv] (default grid) |
| `max_rows` | integer |  | stop after this many rows (default 200) |

#### office.sheet_set
Write cells. value= goes into every cell of range: a number, text, TRUE/FALSE, 15%, $1,200, a 2026-03-01 date or a =formula (copied down/across a range with relative references moving, like Excel's fill). csv= pastes a block of comma- or tab-separated rows starting at the range's top-left cell. Everything is recalculated. Data validation rules (office.sheet_validate) are enforced: a stop rule rejects the value, warning rules add a note. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | target cell or range, e.g. B2 or D2:D13 or Sheet2!A1 |
| `value` | string |  | value or =formula |
| `csv` | string |  | rows of CSV/TSV text to paste at range's top-left |
| `csv_file` | string |  | read the CSV/TSV text from this file instead |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_clear
Clear a range: everything, only the contents, or only the formatting (number format and cell style). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to clear, e.g. B2:D10 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `what` | string |  | what to clear [all\|contents\|formats] (default all) |

#### office.sheet_fill
Fill a range from its first cell(s), like dragging the fill handle. source= says how many leading cells per column/row are the pattern (default 1). Formulas are copied with relative references moving. series=auto continues what the source shows: two numbers or dates continue their trend (1, 3 -> 5, 7), one date counts up a day, dates a month apart keep stepping by months, Item 1 -> Item 2, Jan -> Feb, Mon -> Tue; anything else repeats. step= sets the increment (unit=weekday/month/year for dates), series=growth multiplies, series=copy just repeats. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | range whose first cell/row is the source, e.g. D2:D13 |
| `direction` | string |  | fill down or right (default: down when the range is taller than wide) [down\|right] |
| `source` | integer |  | cells at the start of each column/row that make the pattern (default 1) |
| `series` | string |  | how to continue [auto\|copy\|linear\|growth] (default auto) |
| `step` | number |  | increment per cell (growth: the ratio) |
| `unit` | string |  | date step unit [day\|weekday\|month\|year] (default day) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_format
Format a range: a number format (a preset - accounting, currency, date, datetime, euro, general, integer, number, percent, percent2, pound, scientific, text, time - or a code like #,##0.00, 0.0%, $#,##0, yyyy-mm-dd, "0 kg"), bold on/off, column width in characters, or autofit widths to the contents. Colours, fonts, borders and alignment are office.sheet_style. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to format, e.g. B2:D13 or C:C |
| `format` | string |  | number format preset or code |
| `bold` | boolean |  | bold text |
| `width` | number |  | column width in characters (Excel units, default columns are 8.43) |
| `autofit` | boolean |  | set column widths to fit their contents (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_insert_rows / office.sheet_insert_cols
Insert empty rows before row `at`, or columns before column `at`; formulas that point past them move. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `at` | string | yes | first row number (5), or first column as a letter (C) or number (3) |
| `count` | integer |  | how many (default 1) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_delete_rows / office.sheet_delete_cols
Delete rows or columns starting at `at`; references to deleted cells become #REF!. Undoable. Same parameters as the insert commands.

#### office.sheet_sort
Sort the rows of a range by one or more columns. by= takes column letters or header names, each optionally followed by asc or desc: by="C desc, A". The header row stays on top (detected, or set header=). Numbers sort before text, blanks go last; formulas move with their rows. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | rows to sort (default: the used area) |
| `by` | string | yes | sort keys, e.g. B or "Sales desc, Region" |
| `descending` | boolean |  | default direction for keys without asc/desc (default false) |
| `header` | boolean |  | the first row is a header (default: detected) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_filter
Keep the rows where a column matches a condition. criteria works like COUNTIF: ">100", "<=0", "North", "<>done", "a*" (wildcards), "" (blank). mode=copy (default) writes the header and matching rows as values to a new sheet; mode=delete removes the other rows in place; mode=count only counts. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | table to filter including its header (default: the used area) |
| `column` | string | yes | column letter or header name to test |
| `criteria` | string | yes | condition, e.g. >100 or North |
| `mode` | string |  | what to do with matches [copy\|delete\|count] (default copy) |
| `to` | string |  | mode=copy: name of the new sheet (default: <sheet> filtered) |
| `header` | boolean |  | the first row is a header (default: detected) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_find
Find cells whose displayed value (and optionally formula) contains the text. Lists up to 200 matches. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `query` | string | yes | text to look for |
| `sheet` | string |  | only this sheet (default: all sheets) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `whole_cell` | boolean |  | the whole cell must equal the query (default false) |
| `formulas` | boolean |  | also search formula text (default false) |

#### office.sheet_replace
Replace text inside text cells (and formulas with formulas=true) across the workbook or one sheet. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `find` | string | yes | text to replace |
| `replace` | string |  | replacement (may be empty) |
| `sheet` | string |  | only this sheet (default: all) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `formulas` | boolean |  | also replace inside formulas (default false) |

#### office.sheet_calc
Recalculate every formula, or evaluate a formula without storing it, as if it sat in cell `at` (e.g. formula="SUMIF(A:A,\"North\",C:C)"). Handy to check a result before writing it into the sheet. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `formula` | string |  | formula to evaluate, with or without the leading = |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `at` | string |  | cell the formula is evaluated in (for relative refs) (default A1) |

#### office.sheet_chart
Draw a bar, hbar (horizontal bars), line or pie chart of a range as a PNG. The usual layout is detected: a header row of series names and a first column of category labels (e.g. A1:C13 with months in A and two series in B and C). by_rows=true reads series from rows instead. With path the PNG is written to that file; it is always attached. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | data range including headers/labels, e.g. A1:C13 (default: the used area) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `type` | string |  | chart type [bar\|hbar\|line\|pie] (default bar) |
| `title` | string |  | chart title |
| `by_rows` | boolean |  | series are rows, not columns (default false) |
| `series` | string |  | only these series: header names or numbers, e.g. Total or 1,3 |
| `path` | string |  | write the PNG here (e.g. chart.png) |
| `width` | integer |  | pixels (default 800) |
| `height` | integer |  | pixels (default 500) |

#### office.sheet_preview
Render cells as a PNG grid with column letters and row numbers, the way a spreadsheet app shows them. Without range: the used area (up to 50 rows x 20 columns). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | cells to show, e.g. A1:H30 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `scale` | number |  | pixels per point (default 1.5) |

#### office.sheet_style
Change how cells look: bold/italic/underline/strike, font colour and size, fill colour, horizontal and vertical alignment, wrap, indent and borders (all, outline, inside, single sides). like=A1 copies another cell's look (format painter); clear=true starts from plain. Number formats are office.sheet_format. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to style, e.g. A1:E1 or B:B |
| `bold` | boolean |  | bold text |
| `italic` | boolean |  | italic text |
| `underline` | boolean |  | underlined text |
| `strike` | boolean |  | struck-through text |
| `color` | string |  | font colour: #rrggbb, a name (red, darkgreen, navy, ...) or none |
| `fill` | string |  | background colour: #rrggbb, a name (lightyellow, lightgreen, ...) or none |
| `font_size` | number |  | font size in points (0 = the default 11) |
| `align` | string |  | horizontal alignment [general\|left\|center\|right] |
| `valign` | string |  | vertical alignment [bottom\|middle\|top] |
| `wrap` | boolean |  | wrap text onto several lines inside the cell |
| `indent` | integer |  | indent level (0-15) |
| `border` | string |  | borders to draw: all, outline, inside, top, bottom, left, right, horizontal, vertical or none (comma-separated) |
| `border_style` | string |  | line style for border= [thin\|medium\|thick\|double\|dashed\|dotted] (default thin) |
| `border_color` | string |  | border colour (default black) |
| `like` | string |  | copy the style and number format of this cell first (format painter), e.g. A1 |
| `clear` | boolean |  | reset the cells to the plain style before applying the rest (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_merge
Merge a range into one cell (the top-left value stays; other values in the range are cleared) or unmerge it. center=true (default) also centres the text, like Merge & Center; across=true merges each row on its own. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to merge, e.g. A1:E1 |
| `unmerge` | boolean |  | split merged cells in the range back up (default false) |
| `center` | boolean |  | centre the merged text (default true) |
| `across` | boolean |  | merge each row of the range separately (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_freeze
Keep rows at the top and/or columns at the left in view while scrolling. at=B2 freezes everything above and left of B2 (row 1 and column A); rows=1 freezes the header row; at=A1 or none=true unfreezes. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `at` | string |  | first cell that scrolls, e.g. B2 (A2 = freeze row 1) |
| `rows` | integer |  | rows to freeze at the top |
| `cols` | integer |  | columns to freeze at the left |
| `none` | boolean |  | unfreeze (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_hide
Hide (or with hidden=false show again) whole rows or columns. Hidden rows are skipped by SUBTOTAL/AGGREGATE and in printouts. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `rows` | string |  | rows, e.g. 4 or 4:9 |
| `cols` | string |  | columns, e.g. C or C:E |
| `hidden` | boolean |  | hide (false shows them) (default true) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_row_height
Set the height of rows in points (the default is 15), or autofit=true to fit wrapped and multi-line text. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `rows` | string | yes | rows, e.g. 1 or 2:20 |
| `height` | number |  | height in points |
| `autofit` | boolean |  | fit the contents (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_name
Give a range or value a name to use in formulas: name=TaxRate refers_to=B1 makes =A2*TaxRate work; refers_to may be a range (Sales!C2:C100), a constant (0.2) or any formula. Plain cell references are made absolute on the given sheet. delete=true removes the name (formulas using it then give #NAME?). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `name` | string | yes | the name, e.g. TaxRate (letters, digits, _ and .; not a cell reference) |
| `refers_to` | string |  | range, constant or formula |
| `comment` | string |  | a note about the name |
| `delete` | boolean |  | remove the name (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_names
List the workbook's defined names (named ranges and constants) with what they refer to and their current value. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |

#### office.sheet_cond_format
Highlight cells by rule, like Excel's conditional formatting: type=cell op=">" value=100 fill=lightgreen; type=text value=late color=red; type=top value=5; above_average, below_average, duplicate, unique, blank, not_blank, errors; type=formula value="$C2>$D2" (relative refs move per cell); color_scale (2 or 3 colours) and data_bar shade by value. Later rules win where they overlap. Saved in .xlsx. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells the rule covers, e.g. B2:B100 |
| `type` | string |  | kind of rule [cell\|formula\|text\|begins\|ends\|top\|bottom\|above_average\|below_average\|duplicate\|unique\|blank\|not_blank\|errors\|color_scale\|data_bar] (default cell) |
| `op` | string |  | type=cell: comparison [>\|<\|>=\|<=\|=\|<>\|between\|not_between] (default >) |
| `value` | string |  | type=cell: the bound (number, date, text or formula like $B$1); type=formula: a formula that is TRUE for cells to format, written for the range's top-left cell (e.g. $C2>$D2); text/begins/ends: the text; top/bottom: how many (default 10) |
| `value2` | string |  | upper bound for between / not_between |
| `percent` | boolean |  | top/bottom: value is a percentage (default false) |
| `bold` | boolean |  | bold text |
| `italic` | boolean |  | italic text |
| `underline` | boolean |  | underlined text |
| `strike` | boolean |  | struck-through text |
| `color` | string |  | font colour: #rrggbb, a name (red, darkgreen, navy, ...) or none |
| `fill` | string |  | background colour: #rrggbb, a name (lightyellow, lightgreen, ...) or none |
| `font_size` | number |  | font size in points (0 = the default 11) |
| `align` | string |  | horizontal alignment [general\|left\|center\|right] |
| `valign` | string |  | vertical alignment [bottom\|middle\|top] |
| `wrap` | boolean |  | wrap text onto several lines inside the cell |
| `indent` | integer |  | indent level (0-15) |
| `border` | string |  | draw a border around matching cells: thin, medium, thick or none |
| `min_color` | string |  | color_scale: colour of the lowest value (default red #f8696b) |
| `mid_color` | string |  | color_scale: colour of the middle value (optional, makes a 3-colour scale) |
| `max_color` | string |  | color_scale: colour of the highest value (default green #63be7b); data_bar: the bar colour |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_cond_formats
List a sheet's conditional formatting rules in order (later ones win), with how many cells each highlights now. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_cond_clear
Remove conditional formatting rules: rule=N (from office.sheet_cond_formats), every rule touching range=, or all of the sheet's. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `rule` | integer |  | rule number |
| `range` | string |  | remove the rules that overlap these cells |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_validate
Limit what may be typed into cells: type=list (value="North,South" or a range like $F$1:$F$9; shows a dropdown), whole or decimal numbers (op=between value=0 value2=100, or op=">=" value=0), date, time, length (of the text) or custom (value=a formula TRUE for good input, e.g. COUNTIF($A:$A,A2)=1). error_style=stop rejects bad input in office.sheet_set; warning/information accept it with a note. Saved in .xlsx. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells the rule covers, e.g. B2:B100 |
| `type` | string |  | what is allowed [list\|whole\|decimal\|date\|time\|length\|custom] (default list) |
| `op` | string |  | comparison for numbers, dates, times and lengths [>\|<\|>=\|<=\|=\|<>\|between\|not_between] (default between) |
| `value` | string | yes | list items, a list range, the (lower) bound or the custom formula |
| `value2` | string |  | upper bound for between |
| `allow_blank` | boolean |  | empty cells are fine (default true) |
| `dropdown` | boolean |  | type=list: show the in-cell dropdown (default true) |
| `error_style` | string |  | what happens to bad input [stop\|warning\|information] (default stop) |
| `error_title` | string |  | title of the error message |
| `error` | string |  | error message (default: says what is allowed) |
| `prompt_title` | string |  | title of the input hint |
| `prompt` | string |  | input hint shown when the cell is selected |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_validations
List a sheet's data validation rules and the cells that currently break them (like Excel's Circle Invalid Data). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_validation_clear
Remove data validation from range= (rules overlapping it) or from the whole sheet. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | cells to free (default: every rule on the sheet) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_chart_add
Put a chart on the sheet over the cells (it follows the data, is drawn in previews and PDFs and is saved as a real chart in .xlsx). The data range is read like office.sheet_chart: a header row of series names and a first column of categories. Types: bar, hbar, line, area, pie, scatter (first column = x values). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | data including headers, e.g. A1:C13 |
| `type` | string |  | chart type [bar\|hbar\|line\|area\|pie\|scatter] (default bar) |
| `title` | string |  | chart title |
| `by_rows` | boolean |  | series are rows, not columns (default false) |
| `at` | string |  | top-left cell the chart sits on (default: two columns right of the data) |
| `width` | integer |  | pixels (default 480) |
| `height` | integer |  | pixels (default 288) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_chart_edit
Change a chart on the sheet: its type, title, data range, position or size. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `chart` | integer |  | chart number (default: the last one) |
| `type` | string |  | chart type [bar\|hbar\|line\|area\|pie\|scatter] |
| `title` | string |  | chart title |
| `range` | string |  | data range |
| `by_rows` | boolean |  | series are rows |
| `at` | string |  | top-left cell |
| `width` | integer |  | pixels |
| `height` | integer |  | pixels |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_chart_delete
Remove a chart from the sheet. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `chart` | integer |  | chart number (default: the last one) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_charts
List the charts placed on a sheet. chart=N attaches that chart as a PNG. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `chart` | integer |  | attach this chart's picture |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_pivot
Summarise a table by category, like a pivot table: rows=Region values=Sales sums Sales per region; add columns=Quarter for a cross-tab; function=count|average|min|max. The cells are live SUMIFS/COUNTIFS/AVERAGEIFS formulas, so the summary updates when the data changes (static=true writes plain values). Totals are added. Goes to a new sheet unless to= names a cell. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | the table including its header row (default: the used area) |
| `rows` | string | yes | field for the row labels (header name or column letter) |
| `columns` | string |  | field for the column labels (optional) |
| `values` | string |  | field to summarise (count needs none) |
| `function` | string |  | how to summarise [sum\|count\|average\|min\|max] (default sum) |
| `to` | string |  | top-left cell for the result, e.g. H1 or Summary!A1 (default: a new sheet) |
| `static` | boolean |  | write values instead of formulas (default false) |
| `totals` | boolean |  | add grand totals (default true) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_copy
Copy a range to another place (same or another sheet) like copy and paste: relative references in formulas move with the cells, absolute ($A$1) ones stay. what=values pastes the computed values only, formulas pastes without formats, formats pastes only the look (format painter for a block). transpose=true turns rows into columns; skip_blanks=true leaves cells under empty source cells alone. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `from` | string | yes | cells to copy, e.g. A1:D10 |
| `to` | string | yes | top-left cell of the destination, e.g. F1 or Sheet2!A1 |
| `what` | string |  | what to paste [all\|values\|formulas\|formats] (default all) |
| `transpose` | boolean |  | swap rows and columns (default false) |
| `skip_blanks` | boolean |  | don't overwrite with empty cells (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_move
Cut and paste a range: the cells (with formats and merges) move to `to`, and every formula that referred to them follows, as in Excel. Whatever was at the destination is replaced. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `from` | string | yes | cells to move, e.g. A1:D10 |
| `to` | string | yes | new top-left cell, e.g. F1 or Sheet2!A1 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_autofilter
Filter a table in place, like Excel's AutoFilter: rows whose column doesn't match the criteria are hidden (the data stays; SUBTOTAL and AGGREGATE skip hidden rows). Each call adds a condition on one column; clear=true shows every row again and removes the filter. Criteria work like COUNTIF: ">100", "North", "<>done", "a*". Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | the table including its header row (default: the used area, or the current filter) |
| `column` | string |  | column letter or header name |
| `criteria` | string |  | condition the kept rows meet |
| `clear` | boolean |  | show all rows and remove the filter (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_import
Import a CSV/TSV file (or text) into the open workbook with import options, like Excel's text import: delimiter, rows to skip, decimal comma, day-first or month-first dates, columns kept as text, encoding. Goes to a new sheet unless at= names a cell. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | CSV/TSV/text file to import |
| `csv` | string |  | or the CSV text itself |
| `name` | string |  | import into a new sheet with this name (default: the file name) |
| `at` | string |  | or paste into an existing sheet starting at this cell, e.g. Sheet1!A20 |
| `delimiter` | string |  | CSV delimiter [auto\|comma\|semicolon\|tab\|pipe\|space] (default auto) |
| `skip_rows` | integer |  | lines to skip at the top (titles above the table) |
| `decimal_comma` | boolean |  | numbers use a decimal comma: 1.234,5 = 1234.5 |
| `date_order` | string |  | how 03/04/2026 reads [auto\|dmy\|mdy] (default auto) |
| `text_columns` | string |  | columns kept as text (ids with leading zeros), e.g. A,C |
| `detect_types` | boolean |  | turn numbers, dates and TRUE/FALSE into values (false keeps every field as text) |
| `encoding` | string |  | text encoding [auto\|utf-8\|latin1] (default auto) |

#### Sheets in a workbook

| Command | Parameters | What it does |
| --- | --- | --- |
| `office.sheet_add` | `name` (default SheetN), `at` (position, default last), `csv`, `csv_file` | add a sheet and make it active, optionally filled from CSV/TSV. Undoable |
| `office.sheet_rename` | `sheet` (required), `name` (required) | rename a sheet; formulas that refer to it are updated. Undoable |
| `office.sheet_delete` | `sheet` (required) | delete a sheet; references to it become #REF!. Undoable |
| `office.sheet_select` | `sheet` (required) | make a sheet the active one (the default for other commands and for CSV saves). Undoable |

### Documents

#### office.doc_new
Create a document and return its id (doc1, ...). markdown= fills it straight away (# headings, **bold**, *italic*, [links](url), - lists, 1. numbered lists, | tables |, > quotes, ``` code, ![images](file.png), <!-- pagebreak -->). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `markdown` | string |  | initial content as Markdown |
| `markdown_file` | string |  | read the Markdown from this file (images relative to it) |
| `page` | string |  | paper size [a4\|letter\|legal\|a5\|a3] |
| `landscape` | boolean |  | landscape orientation |
| `margin` | number |  | page margin in mm (all sides, default 25.4) |
| `margin_top` | number |  | top margin in mm |
| `margin_bottom` | number |  | bottom margin in mm |
| `margin_left` | number |  | left margin in mm |
| `margin_right` | number |  | right margin in mm |
| `font` | string |  | body font [sans\|serif\|mono] |
| `font_size` | number |  | body text size in pt (default 11) |
| `page_numbers` | boolean |  | "n / N" page numbers in the footer |
| `title` | string |  | document title (file metadata) |
| `author` | string |  | author (file metadata) |

#### office.doc_open
Open a document: .docx and .odt (styles, run and paragraph formats, lists, tables with merged cells, images, headers and footers, footnotes, comments, tracked changes, table of contents), .md, .html, .txt; .doc, .rtf and others through LibreOffice. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |

#### office.doc_save
Save the document; the extension picks the format: .docx (Word), .md (images go to <name>_media/), .html (one self-contained file), .odt (OpenDocument; tracked changes are saved accepted), .txt, .pdf; .rtf, .doc through LibreOffice. Without path it saves back to its own file. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the document's own path) |

#### office.doc_export
Export to another file without changing which file the document is saved to: .pdf (laid out here: fonts, word wrap, lists, tables, images, headers, footers, footnotes, table of contents), .docx, .odt, .md, .html, .txt, .png/.jpg (one page, page= and dpi=); .rtf/.doc through LibreOffice. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `path` | string | yes | output file; the extension picks the format |
| `page` | integer |  | png/jpg: which page (default 1) |
| `dpi` | number |  | png/jpg resolution (default 110) |

#### office.doc_close
Close a document without saving (undo reopens it). Undoable.

#### office.doc_info
Describe a document: title, author, page setup, page count, word count and every block with its number (the numbers doc_edit, doc_delete, doc_move and doc_add at= use). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `max_blocks` | integer |  | list at most this many blocks (default 200) |

#### office.doc_get_text
Read the document's content as Markdown (default) or plain text, each block prefixed with its number [n] unless numbered=false. from/to pick a block range. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `format` | string |  | markdown keeps styles, text is plain [markdown\|text] (default markdown) |
| `numbered` | boolean |  | prefix each block with [n] (default true) |
| `from` | integer |  | first block (1-based) |
| `to` | integer |  | last block |

#### office.doc_add
Add content. kind=paragraph|heading|quote take inline Markdown text (**bold**, *italic*, <u>underline</u>, `code`, [link](url)); list/numbered take one item per line (indent 2 spaces for sub-items); table takes one row per line with cells split by |, tabs or commas (first row is the header); code keeps text as-is; image needs path=; pagebreak needs nothing; markdown adds every block of a Markdown text. at= inserts before that block number (default: at the end). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `kind` | string |  | what to add [paragraph\|heading\|list\|numbered\|table\|image\|code\|quote\|pagebreak\|markdown] (default paragraph) |
| `text` | string |  | the content |
| `text_file` | string |  | read the content from this file |
| `level` | integer |  | heading level 1-6 (default 1) |
| `at` | integer |  | insert before this block number (default: append) |
| `path` | string |  | image file (kind=image) |
| `width` | number |  | image width in pt (default: natural size, at most the text width) |
| `alt` | string |  | image description |
| `header` | boolean |  | table: the first row is a header (default true) |
| `numbering` | string |  | list numbering [bullet\|decimal\|lowerLetter\|upperLetter\|lowerRoman\|upperRoman] |
| `start` | integer |  | numbered list: first number (default 1) |

#### office.doc_edit
Change a block: new text (same rules as doc_add for its kind), a different kind (e.g. paragraph -> heading), a heading level, or for images a new path/width/alt. Fields left out stay as they are. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | block number (office.doc_info lists them) |
| `text` | string |  | new content |
| `text_file` | string |  | read the new content from this file |
| `kind` | string |  | change the block into this kind [paragraph\|heading\|list\|numbered\|table\|image\|code\|quote\|pagebreak\|markdown] |
| `level` | integer |  | heading level 1-6 |
| `path` | string |  | image file |
| `width` | number |  | image width in pt |
| `alt` | string |  | image description |
| `header` | boolean |  | table: first row is a header |
| `numbering` | string |  | list numbering [bullet\|decimal\|lowerLetter\|upperLetter\|lowerRoman\|upperRoman] |
| `start` | integer |  | numbered list: first number |

#### office.doc_delete / office.doc_move
`office.doc_delete` deletes `block` (required), or `count` blocks starting there; with track changes on, their text is marked deleted until accepted. `office.doc_move` moves `block` (required) so it ends up at position `to` (required). Both undoable.

#### office.doc_find_replace
Find text everywhere in the document (headings, paragraphs, lists, tables, quotes, code) and replace it, keeping each run's style. Without replace it only counts and lists where the text is. A match must sit inside one style run. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `find` | string | yes | text to find |
| `replace` | string |  | replacement (may be empty) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `whole_word` | boolean |  | only whole words (default false) |

#### office.doc_setup
Change the paper size, orientation, margins, body font and size, page numbers, title or author. Undoable. Takes the same `page`, `landscape`, `margin`, `margin_top`, `margin_bottom`, `margin_left`, `margin_right`, `font`, `font_size`, `page_numbers`, `title` and `author` parameters as `office.doc_new` (margins in mm).

#### office.doc_word_count
Count words, characters (with and without spaces), paragraphs and pages, with a reading time. Read-only. Takes `file` for one-shot use.

#### office.doc_preview
Render one page as a PNG, exactly as the PDF export lays it out. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `page` | integer |  | page number (default 1) |
| `scale` | number |  | pixels per point (default 1.25) |

#### office.doc_format
Format a block (or blocks block..to): character formatting (bold, italic, underline, strike, color, highlight, size, font, superscript/subscript, link) on all its text or only on find= matches, list item= or table row=/col=; paragraph formatting (align, indents, spacing, line spacing, keep with next, page break before); style= applies a paragraph style (Heading 2 turns the block into a heading). align also places images. clear=true removes direct formatting first. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | block number (office.doc_info lists them) |
| `to` | integer |  | format blocks block..to |
| `find` | string |  | only this text (every match); default: all the text |
| `match_case` | boolean |  | find: case-sensitive (default false) |
| `whole_word` | boolean |  | find: whole words only (default false) |
| `item` | integer |  | list: only this item (1-based) |
| `row` | integer |  | table: only this row (1-based) |
| `col` | integer |  | table: only this column (1-based) |
| `bold` | boolean |  | bold |
| `italic` | boolean |  | italic |
| `underline` | boolean |  | underline |
| `strike` | boolean |  | strikethrough |
| `code` | boolean |  | code (monospace) text |
| `color` | string |  | text colour (#rrggbb or a name; none = automatic) |
| `highlight` | string |  | highlight colour behind the text (none removes it) |
| `size` | number |  | text size in pt (0 = the paragraph's) |
| `font` | string |  | font [sans\|serif\|mono\|default] |
| `position` | string |  | raised or lowered text [normal\|superscript\|subscript] |
| `link` | string |  | make the text a link to this URL (empty removes the link) |
| `style` | string |  | paragraph style (Normal, Title, Subtitle, Heading 1-6, Quote, Code, Caption, or a style made with office.doc_style_define) |
| `align` | string |  | paragraph alignment [left\|center\|right\|justify] |
| `indent` | number |  | left indent in pt |
| `indent_right` | number |  | right indent in pt |
| `first_line` | number |  | first-line indent in pt (negative: hanging) |
| `space_before` | number |  | space above in pt |
| `space_after` | number |  | space below in pt |
| `line_spacing` | number |  | line spacing as a multiple (1 single, 1.5, 2 double) |
| `keep_with_next` | boolean |  | keep on the same page as the next paragraph |
| `page_break_before` | boolean |  | start the block on a new page |
| `clear` | boolean |  | first remove direct formatting (run styles and paragraph format) |

#### office.doc_style_define
Create a paragraph style or change one (built-in or your own): font, size, colour, bold/italic/underline, alignment, indents, spacing. Every block with the style follows. Apply it with office.doc_format style=. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `name` | string | yes | style name: a built-in (Normal, Title, Heading 1, Quote, ...) to change it, or a new name |
| `based_on` | string |  | style it starts from (default Normal) (Normal, Title, Subtitle, Heading 1-6, Quote, Code, Caption, or a style made with office.doc_style_define) |
| `font` | string |  | font [sans\|serif\|mono\|default] |
| `size` | number |  | text size in pt |
| `color` | string |  | text colour (#rrggbb or a name) |
| `bold` | boolean |  | bold |
| `italic` | boolean |  | italic |
| `underline` | boolean |  | underline |
| `align` | string |  | paragraph alignment [left\|center\|right\|justify] |
| `indent` | number |  | left indent in pt |
| `indent_right` | number |  | right indent in pt |
| `first_line` | number |  | first-line indent in pt (negative: hanging) |
| `space_before` | number |  | space above in pt |
| `space_after` | number |  | space below in pt |
| `line_spacing` | number |  | line spacing as a multiple (1 single, 1.5, 2 double) |
| `keep_with_next` | boolean |  | keep on the same page as the next paragraph |
| `reset` | boolean |  | built-in: back to its default look; your own: delete it (blocks using it become Normal) |

#### office.doc_styles
List the paragraph styles: the built-in ones and your own, how each looks and how many blocks use it. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |

#### office.doc_list
Change a list: numbering (bullet, decimal 1., lowerLetter a), upperLetter A., lowerRoman i., upperRoman I.), the first number, or one item's level (item= with level= 1 for top, 2 for a sub-item, ...). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the list's block number |
| `numbering` | string |  | list numbering [bullet\|decimal\|lowerLetter\|upperLetter\|lowerRoman\|upperRoman] |
| `start` | integer |  | first number |
| `item` | integer |  | item to move in or out (1-based) |
| `level` | integer |  | the item's level, 1 = top |

#### office.doc_table_cell
Set a table cell's text (inline Markdown; a line break makes a new line in the cell) and/or format cells: fill colour, horizontal align and vertical align. to_row/to_col format a whole range of cells. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the table's block number |
| `row` | integer | yes | row (1-based) |
| `col` | integer | yes | column (1-based) |
| `to_row` | integer |  | last row of a range to format |
| `to_col` | integer |  | last column of a range to format |
| `text` | string |  | new cell text |
| `fill` | string |  | background colour (#rrggbb or a name; none removes it) |
| `align` | string |  | text alignment [left\|center\|right] |
| `valign` | string |  | vertical alignment [top\|middle\|bottom] |

#### office.doc_table_merge
Merge a block of table cells into one (rows x cols from row/col; their text joins in the top-left cell), or unmerge=true to split the merged cell at row/col again. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the table's block number |
| `row` | integer | yes | top row (1-based) |
| `col` | integer | yes | left column (1-based) |
| `rows` | integer |  | how many rows (default 1) |
| `cols` | integer |  | how many columns (default 1) |
| `unmerge` | boolean |  | split instead |

#### office.doc_table_insert
Insert rows (row= with where=above|below) or columns (col= with where=left|right) into a table; count= adds several. text= fills a new row (cells split by |) or column (one cell per line). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the table's block number |
| `row` | integer |  | insert next to this row |
| `col` | integer |  | insert next to this column |
| `where` | string |  | which side (default below / right) [above\|below\|left\|right] |
| `count` | integer |  | how many (default 1) |
| `text` | string |  | content of the new row or column |

#### office.doc_table_delete
Delete table rows (row=) or columns (col=), count= of them. A table keeps at least one row and column (office.doc_delete removes the whole table). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the table's block number |
| `row` | integer |  | first row to delete |
| `col` | integer |  | first column to delete |
| `count` | integer |  | how many (default 1) |

#### office.doc_table_format
Table-wide settings: header row, grid lines (borders), banded rows, and column widths as relative numbers (widths=2,1,1) or widths=auto to fit the contents. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | the table's block number |
| `header` | boolean |  | the first row is a header |
| `borders` | boolean |  | grid lines |
| `banded` | boolean |  | shade every other row |
| `widths` | string |  | relative column widths, e.g. 3,1,1 (auto = fit the contents) |

#### office.doc_header_footer
Set the page header and footer: inline Markdown with fields {page}, {pages}, {title}, {author}, {date}; "left | center | right" places three parts ("a | b" left and right; no | centres it). An empty value removes it. different_first=true gives the first page its own first_header/first_footer (empty = nothing on the title page). Without a footer, page numbers show when page_numbers is on (office.doc_setup). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `header` | string |  | header text |
| `footer` | string |  | footer text |
| `different_first` | boolean |  | the first page has its own header and footer |
| `first_header` | string |  | first page header |
| `first_footer` | string |  | first page footer |

#### office.doc_toc
Insert a table of contents (headings down to levels=, with page numbers; it updates itself as headings change), or change the one there (levels=, title=), or remove=true. at= puts it before that block (default: the top, after a Title paragraph). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `at` | integer |  | insert before this block number |
| `levels` | integer |  | deepest heading level listed (1-6) (default 3) |
| `title` | string |  | heading above it (default Contents) |
| `remove` | boolean |  | remove the table of contents |

#### office.doc_footnote
Add a footnote: a numbered mark in the text (after= some text, or at the end of the block / list item= / table cell row= col=) and the note at the foot of the page. number= changes an existing footnote's text instead. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer |  | block to put the note mark in |
| `text` | string | yes | the note (inline Markdown) |
| `after` | string |  | put the mark right after this text (default: at the end) |
| `item` | integer |  | list: only this item (1-based) |
| `row` | integer |  | table: only this row (1-based) |
| `col` | integer |  | table: only this column (1-based) |
| `number` | integer |  | change footnote n's text instead of adding one |

#### office.doc_footnotes
List the footnotes in order with their numbers and blocks. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |

#### office.doc_footnote_delete
Delete footnote n (its mark and its note); the others renumber. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `number` | integer | yes | footnote number (office.doc_footnotes lists them) |

#### office.doc_comment
Add a comment on a block, on some text in it (find=), on a list item= or a table cell row=/col=. Comments show highlighted in previews and PDFs (show_markup) and are saved in .docx and .odt. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | block to comment on |
| `text` | string | yes | the comment |
| `find` | string |  | comment on this text only (default: the whole block) |
| `author` | string |  | who writes it (default: you) |
| `item` | integer |  | list: only this item (1-based) |
| `row` | integer |  | table: only this row (1-based) |
| `col` | integer |  | table: only this column (1-based) |

#### office.doc_comments
List the comments: number, author, open or resolved, the text they are on and what they say. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `open_only` | boolean |  | only comments not resolved (default false) |

#### office.doc_comment_resolve
Mark a comment resolved (resolved=false opens it again). Resolved comments are not highlighted. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `id` | integer | yes | comment number (office.doc_comments lists them) |
| `resolved` | boolean |  | resolved (default true) |

#### office.doc_comment_delete
Delete a comment (id=), or every comment with all=true. The text stays. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `id` | integer |  | comment number |
| `all` | boolean |  | delete every comment |

#### office.doc_track
Turn track changes on or off (on=), choose whether previews and PDFs show the markup (show_markup=: insertions underlined, deletions struck through, commented text highlighted) and who the edits are by (author=). While on, doc_add, doc_edit, doc_delete, doc_find_replace, doc_table_cell text and doc_footnote record insertions and deletions. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `on` | boolean |  | record changes |
| `show_markup` | boolean |  | show changes and comments in previews and PDFs |
| `author` | string |  | name on new changes and comments |

#### office.doc_changes
List the tracked changes waiting to be accepted or rejected: number, insertion or deletion, author, date and text. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |

#### office.doc_accept
Accept a tracked change (id=) or all of them: insertions stay as normal text, deletions go. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `id` | integer |  | change number (office.doc_changes lists them; default all) |

#### office.doc_reject
Reject a tracked change (id=) or all of them: insertions go, deleted text comes back. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `id` | integer |  | change number (office.doc_changes lists them; default all) |

### Slide decks

#### office.deck_new
Create a 16:9 slide deck and return its id (deck1, ...). outline= builds every slide from Markdown at once (see the outline syntax in task 4). A first slide without bullets becomes the title slide. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `title` | string |  | deck title (file metadata) |
| `author` | string |  | author |
| `theme` | string |  | colour theme: blue, teal, green, orange, red, purple, slate, dark, or an accent colour like #0a7d5a (default blue) |
| `font` | string |  | font family [sans\|serif\|mono] (default sans) |
| `outline` | string |  | slides as a Markdown outline |
| `outline_file` | string |  | read the outline from this .md file |
| `aspect` | string |  | slide shape [16:9\|4:3] (default 16:9) |

#### office.deck_open
Open a deck: .pptx (titles, subtitles, bullets with levels, pictures, text boxes, shapes and lines, speaker notes, transitions, hidden slides, theme colours and font, the master's shapes, footer and slide numbers) or a Markdown outline (.md); .odp and .ppt through LibreOffice. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |

#### office.deck_save
Save the deck: .pptx (PowerPoint, Keynote, Google Slides and LibreOffice open it), .pdf (one page per slide), .md (outline; pictures go to <name>_media/); .odp/.ppt through LibreOffice. Without path it saves back to its own file. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the deck's own path) |

#### office.deck_export
Export without changing the deck's own file: .pdf (notes=true: notes pages, the slide above its speaker notes), .pptx, .md, .html (a slide show to present from a browser, like office.deck_present), .odp/.ppt (LibreOffice), or .png/.jpg - one slide with slide=, otherwise every slide as name-1.png, name-2.png, ... Hidden slides are left out of .pdf and .html. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `path` | string | yes | output file; the extension picks the format |
| `slide` | integer |  | png: only this slide |
| `width` | integer |  | image width in pixels (png default 1280, html 1600) |
| `notes` | boolean |  | pdf: notes pages with the speaker notes (default false) |
| `loop` | boolean |  | html: start again after the last slide (default false) |

#### office.deck_close
Close a deck without saving (undo reopens it). Undoable.

#### office.deck_info
List the slides with their layouts, titles, bullet counts, pictures and notes, and warn about slides with too much text. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `outline` | boolean |  | also return the whole deck as a Markdown outline (default false) |

#### office.deck_add_slide
Add a slide. Layouts: title, section, content, two_column, image, image_right, title_only, blank. Without layout: the first slide of an empty deck with no bullets is a title slide, otherwise content (which puts a picture beside the bullets when image= is given). Long bullet lists shrink to fit; info warns when they no longer can. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `layout` | string |  | slide layout [title\|section\|content\|two_column\|image\|image_right\|title_only\|blank] |
| `title` | string |  | slide title (inline Markdown) |
| `subtitle` | string |  | subtitle (title/section slides) or a line under the title |
| `bullets` | string |  | bullet points, one per line; indent 2 spaces for sub-bullets; - or 1. markers optional |
| `bullets_file` | string |  | read the bullets from this file |
| `right` | string |  | two_column: the right column's bullets |
| `image` | string |  | picture file (png/jpg) for image layouts |
| `notes` | string |  | speaker notes |
| `background` | string |  | this slide's background colour (#rrggbb, a name, accent; none = the layout's) |
| `hidden` | boolean |  | hide the slide: left out of shows and PDF exports |
| `at` | integer |  | insert at this position (default: last) |

#### office.deck_edit_slide
Change a slide's layout, title, subtitle, bullets, right column, image or notes; fields left out stay. Undoable. Takes `slide` (required), the same fields as `office.deck_add_slide` except `at`, and `clear_image=true` to remove the picture.

#### office.deck_delete_slide / office.deck_move_slide
`office.deck_delete_slide` deletes `slide` (required). `office.deck_move_slide` moves `slide` (required) so it ends up at position `to` (required). Both undoable.

#### office.deck_theme
Change the colour theme and/or the font of the whole deck; accent=, background= and text_color= set single theme colours (after theme=). Title and section slides use the accent as their background. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `theme` | string |  | colour theme: blue, teal, green, orange, red, purple, slate, dark, or an accent colour like #0a7d5a |
| `font` | string |  | font family [sans\|serif\|mono] |
| `accent` | string |  | accent colour |
| `background` | string |  | background colour of content slides |
| `text_color` | string |  | body text colour |

#### office.deck_preview
Render a slide (or all=true for every slide) as PNG, exactly as the PDF export draws it. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `slide` | integer |  | slide number (default 1) |
| `all` | boolean |  | attach every slide (default false) |
| `width` | integer |  | pixels (default 960) |

#### office.deck_text_box
Put a text box anywhere on a slide (or on the master, for every slide). Without w it is 400 points wide; without h it grows with its text. Without x/y or position it goes in the middle. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `x` | number |  | left edge in points from the slide's left (the slide is 960 x 540 points, 720 x 540 at 4:3) |
| `y` | number |  | top edge in points from the slide's top |
| `w` | number |  | width in points |
| `h` | number |  | height in points (text boxes: 0 = grow with the text) |
| `position` | string |  | place it on the slide instead of x/y (48 pt from the edges) [center\|top\|bottom\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right] |
| `name` | string |  | a name to find it by later (instead of its number) |
| `text` | string |  | the text (inline Markdown: **bold**, *italic*, [links](url)); one paragraph per line |
| `size` | number |  | text size in points (default 20 in text boxes, 18 in shapes) |
| `color` | string |  | text colour (default: readable on the fill) |
| `bold` | boolean |  | bold text |
| `italic` | boolean |  | italic text |
| `align` | string |  | horizontal alignment [left\|center\|right] |
| `valign` | string |  | vertical alignment [top\|middle\|bottom] |
| `bullets` | boolean |  | lines are bullet points (indent 2 spaces for sub-bullets, 1. for numbers) |
| `fill` | string |  | fill colour: #rrggbb, a name, accent, text, background or none |
| `line` | string |  | outline colour (none = no outline) |
| `line_width` | number |  | outline width in points (default 1.5) |

#### office.deck_shape
Draw a shape (rect, rounded, ellipse, triangle, right_triangle, diamond, pentagon, hexagon, octagon, star, arrow_right, arrow_left, arrow_up, arrow_down, chevron, parallelogram, trapezoid, plus, callout) on a slide or the master, optionally with text inside (centred). Default 200 x 120 points in the middle, filled with the theme accent. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `shape` | string |  | the shape [rect\|rounded\|ellipse\|triangle\|right_triangle\|diamond\|pentagon\|hexagon\|octagon\|star\|arrow_right\|arrow_left\|arrow_up\|arrow_down\|chevron\|parallelogram\|trapezoid\|plus\|callout] (default rect) |
| `x` | number |  | left edge in points from the slide's left (the slide is 960 x 540 points, 720 x 540 at 4:3) |
| `y` | number |  | top edge in points from the slide's top |
| `w` | number |  | width in points |
| `h` | number |  | height in points (text boxes: 0 = grow with the text) |
| `position` | string |  | place it on the slide instead of x/y (48 pt from the edges) [center\|top\|bottom\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right] |
| `name` | string |  | a name to find it by later (instead of its number) |
| `text` | string |  | the text (inline Markdown: **bold**, *italic*, [links](url)); one paragraph per line |
| `size` | number |  | text size in points (default 20 in text boxes, 18 in shapes) |
| `color` | string |  | text colour (default: readable on the fill) |
| `bold` | boolean |  | bold text |
| `italic` | boolean |  | italic text |
| `align` | string |  | horizontal alignment [left\|center\|right] |
| `valign` | string |  | vertical alignment [top\|middle\|bottom] |
| `bullets` | boolean |  | lines are bullet points (indent 2 spaces for sub-bullets, 1. for numbers) |
| `fill` | string |  | fill colour: #rrggbb, a name, accent, text, background or none |
| `line` | string |  | outline colour (none = no outline) |
| `line_width` | number |  | outline width in points (default 1.5) |

#### office.deck_line
Draw a straight line or an arrow from (x, y) to (x2, y2), in points, on a slide or the master. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `x` | number | yes | start, from the left |
| `y` | number | yes | start, from the top |
| `x2` | number | yes | end, from the left |
| `y2` | number | yes | end, from the top |
| `color` | string |  | line colour (default: the theme's text colour) |
| `line_width` | number |  | width in points (default 2) |
| `arrow` | boolean |  | an arrowhead at the end (default false) |
| `name` | string |  | a name to find it by later |

#### office.deck_image
Place a picture (png, jpg, ...) anywhere on a slide, or on the master for a logo on every slide. Without w it keeps its natural size up to 480 points wide; h follows the picture's proportions unless given. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `path` | string | yes | picture file |
| `x` | number |  | left edge in points from the slide's left (the slide is 960 x 540 points, 720 x 540 at 4:3) |
| `y` | number |  | top edge in points from the slide's top |
| `w` | number |  | width in points |
| `h` | number |  | height in points (text boxes: 0 = grow with the text) |
| `position` | string |  | place it on the slide instead of x/y (48 pt from the edges) [center\|top\|bottom\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right] |
| `name` | string |  | a name to find it by later (instead of its number) |
| `line` | string |  | border colour (default none) |
| `line_width` | number |  | border width in points |

#### office.deck_object_edit
Change a text box, shape, line or picture on a slide or the master: move it (x/y or position), resize it, change its text, colours or shape, swap the picture (path=), rename it, or bring it forward/back. Fields left out stay. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `object` | string | yes | object number (office.deck_objects lists them) or name |
| `shape` | string |  | change the shape [rect\|rounded\|ellipse\|triangle\|right_triangle\|diamond\|pentagon\|hexagon\|octagon\|star\|arrow_right\|arrow_left\|arrow_up\|arrow_down\|chevron\|parallelogram\|trapezoid\|plus\|callout] |
| `x` | number |  | left edge in points from the slide's left (the slide is 960 x 540 points, 720 x 540 at 4:3) |
| `y` | number |  | top edge in points from the slide's top |
| `w` | number |  | width in points |
| `h` | number |  | height in points (text boxes: 0 = grow with the text) |
| `position` | string |  | place it on the slide instead of x/y (48 pt from the edges) [center\|top\|bottom\|left\|right\|top_left\|top_right\|bottom_left\|bottom_right] |
| `name` | string |  | a name to find it by later (instead of its number) |
| `text` | string |  | the text (inline Markdown: **bold**, *italic*, [links](url)); one paragraph per line |
| `size` | number |  | text size in points (default 20 in text boxes, 18 in shapes) |
| `color` | string |  | text colour (default: readable on the fill) |
| `bold` | boolean |  | bold text |
| `italic` | boolean |  | italic text |
| `align` | string |  | horizontal alignment [left\|center\|right] |
| `valign` | string |  | vertical alignment [top\|middle\|bottom] |
| `bullets` | boolean |  | lines are bullet points (indent 2 spaces for sub-bullets, 1. for numbers) |
| `fill` | string |  | fill colour: #rrggbb, a name, accent, text, background or none |
| `line` | string |  | outline colour (none = no outline) |
| `line_width` | number |  | outline width in points (default 1.5) |
| `x2` | number |  | line: end, from the left |
| `y2` | number |  | line: end, from the top |
| `arrow` | boolean |  | line: arrowhead at the end |
| `path` | string |  | picture: a new picture file |
| `order` | string |  | move it in front of or behind the others [front\|back\|forward\|backward] |

#### office.deck_object_delete
Delete an object (or all=true: every object) from a slide or the master. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `object` | string |  | object number or name |
| `all` | boolean |  | delete every object there (default false) |

#### office.deck_objects
List the text boxes, shapes, lines and pictures on a slide (or on the master) with their numbers, names, positions, sizes, colours and text. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number (or master=true) |
| `master` | boolean |  | the master instead of a slide (its objects show on every slide but title and section slides) (default false) |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |

#### office.deck_master
Change what every slide shares: the footer text, slide numbers, and whether master objects (office.deck_shape, deck_text_box or deck_image with master=true, e.g. a logo) also show on title and section slides. clear=true removes the master objects. Without changes it describes the master. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `footer` | string |  | footer text at the bottom left (empty removes it) |
| `slide_numbers` | boolean |  | slide numbers bottom right |
| `on_title` | boolean |  | also show the master, footer and numbers on title and section slides |
| `clear` | boolean |  | remove every master object (default false) |

#### office.deck_transition
Set how a slide comes in during a show (fade, push, wipe, cover, zoom or none) and its timing: advance_after= moves on by itself after that many seconds (0 = on click). all=true sets every slide. PowerPoint keeps them. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer |  | slide number |
| `all` | boolean |  | every slide (default false) |
| `type` | string |  | transition [none\|fade\|push\|wipe\|cover\|zoom] |
| `direction` | string |  | push, wipe, cover: the way the new slide moves [left\|right\|up\|down] |
| `duration` | number |  | seconds the transition takes (default 0.75) |
| `advance_after` | number |  | seconds before moving on by itself; 0 = on click |

#### office.deck_duplicate_slide
Copy a slide, with its objects, notes and transition, right after it (or to position to=). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `slide` | integer | yes | slide to copy |
| `to` | integer |  | position of the copy (default: right after the original) |

#### office.deck_notes
List every slide's speaker notes (office.deck_edit_slide notes= changes them). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |

#### office.deck_present
Make the deck a slide show to present from any browser: one .html file with every visible slide, the transitions and timings, speaker notes (N), a presenter window with the next slide, notes and a timer (P), full screen (F), black or white screen (B/W), and jumping to a slide by typing its number. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `path` | string |  | the .html file (default: next to the deck, <name>-show.html) |
| `width` | integer |  | slide image width in pixels (default 1600) |
| `loop` | boolean |  | start again after the last slide (a kiosk show with advance_after timings) (default false) |
| `start` | integer |  | open at this slide |

### PDF files

All PDF commands except `pdf_info` and `pdf_text` write files and are not undoable. Page selections look like `1-3,7`, `10-`, `last` or `5-1`.

| Command | Parameters (defaults) | What it does |
| --- | --- | --- |
| `office.pdf_info` | `path` (required) | page count, page sizes, PDF version, title/author/producer, whether it is encrypted or has text. Read-only |
| `office.pdf_text` | `path` (required), `pages` (all) | the text of each page; works for text PDFs, not scans. Read-only |
| `office.pdf_merge` | `inputs` (required: files separated by `;` or `,`, each optionally `file.pdf[pages]`), `output` (required) | join PDFs in order |
| `office.pdf_extract` | `path`, `pages`, `output` (all required) | write the chosen pages, in the order given (repeats allowed), to a new PDF |
| `office.pdf_delete_pages` | `path` (required), `pages` (required), `output` (default: overwrite `path`) | remove pages |
| `office.pdf_split` | `path` (required), `every` (1), `ranges` (parts separated by `;`), `output` (name pattern with `{n}`; default `<name>-{n}.pdf` next to the input) | split into several files |
| `office.pdf_rotate` | `path` (required), `angle` (90; also 180, 270, -90), `pages` (all), `output` (default: overwrite `path`) | turn pages clockwise |
| `office.pdf_from_images` | `images` (required: files separated by `;` or `,`), `output` (required), `page` (`fit`, `a4`, `letter`; default fit), `margin` (36 pt on paper), `dpi` (96, for fit) | one image per page; JPEGs are embedded unchanged, paper pages turn to match each image |
| `office.pdf_render` | `path` (required), `page` (1), `dpi` (100), `output` | a page as PNG through pdftoppm, attached and optionally written to `output` |

### Conversion

#### office.convert
Convert an office file in one step; the output's extension picks the format. The built-in pairs (no LibreOffice) are:

- spreadsheets: csv/tsv/xlsx/ods to xlsx/csv/tsv/pdf/html
- documents: docx/md/html/odt/txt to docx/md/html/txt/pdf
- decks: pptx or a Markdown outline to pptx/pdf/md
- pdf to txt (text extraction)

Everything else (doc, xls, ppt, rtf, odt/ods/odp output, ...) goes through LibreOffice when it is installed. When only one side is built in, the suite reads or writes that side itself and LibreOffice does the other. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `input` | string | yes | file to convert |
| `output` | string | yes | file to write; its extension sets the format |
| `backend` | string |  | `auto` uses the built-in engines when they handle the pair; `builtin` refuses anything else; `libreoffice` forces LibreOffice (default auto) |
| `sheet` | integer |  | spreadsheet to csv/tsv: which sheet (1-based; default the active one) |
| `kind` | string |  | how to read a Markdown input: as a document or a slide outline [auto\|doc\|deck] (default auto) |

`convert.file` from the [convert tool](convert.md) also handles office files. `office.convert` is the one to use for office work, because it needs LibreOffice only for the formats that aren't built in.

#### office.formats
List the file formats the office tool reads and writes, which are built in and which need LibreOffice, and whether LibreOffice and pdftoppm are installed. Read-only.

## Limits

- **Scripts in PDFs.** Western European text uses the standard PDF fonts. Greek and Cyrillic are set in the built-in font (Roboto, embedded with only the letters used, so the file stays small) and copy and search correctly. Chinese, Japanese, Korean, Arabic, Hebrew, Indic scripts and emoji still come out as `?` in PDFs and previews. `.docx`, `.xlsx`, `.pptx`, `.md` and `.html` keep every character, so export those through LibreOffice when you need those scripts in a PDF (`office.convert input=report.docx output=report.pdf backend=libreoffice`).
- **Spreadsheets:**
  - Fonts are one sans family in a size of your choice; themes, cell comments, sparklines, slicers, macros and pivot caches are not read or written. A pivot table from Excel opens as its values; `office.sheet_pivot` makes a formula-based one.
  - Charts are column, bar, line, area, pie and scatter. Other kinds in an `.xlsx` from Excel open as the closest of those (a doughnut as a pie, a radar as a line), with their data and title but not their own colours and fonts.
  - `.xlsx` files from Excel open with their values and formulas. Functions the engine doesn't know (LAMBDA and its helpers, cube and database functions, among others) show `#NAME?` until the cell is rewritten.
  - LibreOffice 24.2 and older don't know `SORT`, `SORTBY`, `UNIQUE`, `FILTER` and the other dynamic-array functions, so those cells show `#NAME?` there. Excel and newer LibreOffice show them.
  - In the window, italic text is drawn upright and frozen panes scroll with the rest; both are kept in the file and in PDFs.
  - CSV saves hold one sheet's computed values.
  - Sorting moves formulas with their rows but doesn't rewrite references to those rows from elsewhere.
- **Documents:**
  - Fonts are sans, serif and mono (Liberation/Helvetica, Times, Courier in PDFs). A `.docx` that names other fonts opens with the closest of the three.
  - Text boxes, shapes, columns, sections with different page setups, endnotes (they open as footnotes), comment replies, and fields other than page numbers and the table of contents are not kept from a `.docx` or `.odt`.
  - `.odt` saves tracked changes accepted; `.docx` keeps them. Resolved comments open as open again from `.docx`.
  - A merged cell's text joins the top-left cell; Markdown and HTML can't show merges, so they get the plain grid.
  - With track changes on, edits to lists and tables (other than `doc_table_cell text=`), images and page breaks are not recorded; they just happen.
  - Markdown tables ignore column alignment (`---:`); `office.doc_table_cell align=` sets it.
  - Images are placed inline, one per block, aligned left, centred or right.
- **Slides:**
  - A `.pptx` from elsewhere keeps its pictures, text boxes, shapes and lines where they were, but its titles and bullets are laid out again in the suite's eight layouts, and its own layouts and fonts are not kept. Its master's shapes become master objects.
  - Objects can't be rotated, cropped or grouped here. Rotated shapes from other files open unrotated, groups open as their separate shapes, a gradient opens as its first colour, and shapes other than the nineteen listed open as the nearest one or a rectangle.
  - Charts, tables, SmartArt, video, audio and animations inside a slide are not supported; from a `.pptx` they are left out. Put a chart PNG on the slide instead.
  - Transitions are fade, push, wipe, cover and zoom. Other effects open as the nearest one, and durations are saved as PowerPoint's fast, medium and slow (0.5, 0.75 and 1 second).
  - Text with automatic colour comes back from `.pptx` with that colour fixed.
  - The Markdown outline keeps slides, notes and pictures, but not objects, the master or transitions.
  - The slide show draws each slide as a picture, so its text can't be selected and links don't work there (they do in the PDF and `.pptx`).
- **PDFs:**
  - Merge, split, extract and rotate keep each page as it is, but the new file has no bookmarks and no working form fields.
  - Encrypted PDFs can't be merged, split or read; `office.pdf_info` says when a file is encrypted.
  - Text extraction reads text PDFs, not scans (there is no OCR), and complex layouts may come out in an odd order.
  - `office.pdf_render` needs `pdftoppm` (poppler-utils); the suite's own previews don't.
- **LibreOffice formats** (`.doc`, `.xls`, `.ppt`, `.rtf`, `.odp` output and the rest) need LibreOffice installed. Without it those commands say so and change nothing. The first LibreOffice conversion takes a few seconds while LibreOffice starts.
