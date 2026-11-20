// Implements the native Windows inventory manager and connects its windows to inventory and storage operations.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include "Storage.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sstream>
#include <limits>

namespace {
HINSTANCE instance;
HWND mainWindow, table, tabs, status;
HFONT font;
Inventory inventory;
fs::path folder;
std::vector<Sale> basket;

enum Page { Articles, Categories, Suppliers, Sectors, Checkout, History };
enum Command { Add = 100, Edit, Delete, Import, Finish, CancelSale, OpenFolder, DefaultPrices, CustomPrices };
Page page = Articles;

HWND control(HWND parent, const wchar_t* kind, const Text& text, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    HWND window = CreateWindowExW(0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance, nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return window;
}

Text windowText(HWND window) {
    Text text(GetWindowTextLengthW(window) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}

Text money(double value) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}

Text trim(const Text& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == Text::npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

Text join(const std::vector<Text>& names) {
    Text result;
    for (const auto& name : names) {
        if (!result.empty()) result += L"; ";
        result += name;
    }
    return result;
}

std::vector<Text> split(const Text& value) {
    std::vector<Text> names;
    std::wistringstream input(value);
    Text name;
    while (std::getline(input, name, L';')) {
        name = trim(name);
        if (!name.empty() && std::ranges::find(names, name) == names.end())
            names.push_back(name);
    }
    return names;
}

[[noreturn]] void fail(const char* reason) { throw std::runtime_error(reason); }

int integer(const Text& text, int minimum = 0) {
    std::size_t used = 0;
    const int result = std::stoi(text, &used);
    if (used != text.size() || result < minimum) fail("Enter a valid whole number.");
    return result;
}

double decimal(const Text& text, double minimum = 0, double maximum = 1e9) {
    std::size_t used = 0;
    const double result = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(result) || result < minimum || result > maximum)
        fail("Enter a number within the allowed range.");
    return result;
}

struct Field {
    Text label, value;
    std::vector<Text> options;
    HWND window = nullptr;
};

struct Form {
    Text title;
    std::vector<Field> fields;
    bool accepted = false;
};

LRESULT CALLBACK formProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* form = reinterpret_cast<Form*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        form = static_cast<Form*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(form));
        int y = 14;
        for (auto& field : form->fields) {
            control(window, L"STATIC", field.label, 0, 14, y + 4, 173, 24);
            if (field.options.empty()) {
                field.window = control(window, L"EDIT", field.value,
                    WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 190, y, 280, 26);
            } else {
                field.window = control(window, L"COMBOBOX", L"",
                    WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 190, y, 280, 220);
                int selected = 0;
                for (int i = 0; i < static_cast<int>(field.options.size()); ++i) {
                    SendMessageW(field.window, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(field.options[i].c_str()));
                    if (field.options[i] == field.value) selected = i;
                }
                SendMessageW(field.window, CB_SETCURSEL, selected, 0);
            }
            y += 34;
        }
        control(window, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 290, y, 85, 30, IDOK);
        control(window, L"BUTTON", L"Cancel", WS_TABSTOP, 385, y, 85, 30, IDCANCEL);
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
        for (auto& field : form->fields) field.value = trim(windowText(field.window));
        form->accepted = true;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool prompt(Form& form) {
    RECT parent{};
    GetWindowRect(mainWindow, &parent);
    const int height = 106 + 34 * static_cast<int>(form.fields.size());
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GuitaarForm",
        form.title.c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
        parent.left + (parent.right - parent.left - 500) / 2,
        std::max(0L, parent.top + (parent.bottom - parent.top - height) / 2),
        500, height, mainWindow, nullptr, instance, &form);
    if (!dialog) fail("Could not open the form.");
    EnableWindow(mainWindow, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(form.fields.front().window);
    MSG msg{};
    while (IsWindow(dialog)) {
        const int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(mainWindow, TRUE);
    SetActiveWindow(mainWindow);
    return form.accepted;
}

std::vector<Text> choices(const std::vector<Group>& groups) {
    std::vector<Text> result{L"(none)"};
    for (const auto& group : groups) result.push_back(group.name);
    return result;
}

void editArticle(Inventory& data, int row) {
    Article item;
    if (row >= 0) item = data.articles.at(row);
    const std::vector<Text> pricing{L"Automatic", L"Custom margin", L"Fixed price"};
    Form form{row < 0 ? L"Add article" : L"Edit article", {
        {L"Barcode", row < 0 ? L"" : std::to_wstring(item.barcode)},
        {L"Name", item.name}, {L"Type", item.type}, {L"Notes", item.notes},
        {L"Category", item.category, choices(data.categories)},
        {L"Supplier", item.supplier, choices(data.suppliers)},
        {L"Stock", std::to_wstring(item.stock)}, {L"Purchase price", money(item.cost)},
        {L"Discount (%)", money(item.discount)}, {L"Pricing", pricing.at(item.pricing), pricing},
        {L"Custom margin (%)", money(item.margin)}, {L"Fixed selling price", money(item.price)}
    }};
    if (!prompt(form)) return;
    auto value = [&](int i) -> const Text& { return form.fields[i].value; };
    item.barcode = integer(value(0), 1);
    item.name = value(1);
    if (item.name.empty()) fail("Enter an article name.");
    for (int i = 0; i < static_cast<int>(data.articles.size()); ++i) {
        if (i != row && (data.articles[i].barcode == item.barcode || data.articles[i].name == item.name))
            fail("This barcode or article name already exists.");
    }
    item.type = value(2);
    item.notes = value(3);
    item.category = value(4) == L"(none)" ? L"" : value(4);
    item.supplier = value(5) == L"(none)" ? L"" : value(5);
    item.stock = integer(value(6));
    item.cost = decimal(value(7));
    item.discount = decimal(value(8), 0, 100);
    item.pricing = static_cast<int>(std::ranges::find(pricing, value(9)) - pricing.begin());
    item.margin = decimal(value(10), 0, 10000);
    item.price = decimal(value(11));
    if (row < 0) data.articles.push_back(item);
    else data.articles[row] = item;
}

void editGroup(Inventory& data, int row, bool supplier) {
    auto& rows = supplier ? data.suppliers : data.categories;
    Group group;
    if (row >= 0) group = rows.at(row);
    Form form{supplier ? L"Supplier details" : L"Category details", {
        {L"Name", group.name}, {L"Markup (%)", money(group.margin)},
        {L"Discount (%)", money(group.discount)}
    }};
    if (supplier) form.fields.push_back({L"Contact", group.contact});
    if (!prompt(form)) return;
    const auto oldName = group.name;
    group.name = form.fields[0].value;
    if (group.name.empty() || group.name == L"(none)" || group.name.find(L';') != Text::npos)
        fail("Enter a name without semicolons.");
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (i != row && rows[i].name == group.name) fail("This name already exists.");
    group.margin = decimal(form.fields[1].value, 0, 10000);
    group.discount = decimal(form.fields[2].value, 0, 100);
    if (supplier) group.contact = form.fields[3].value;
    if (row < 0) {
        rows.push_back(group);
        return;
    }
    rows[row] = group;
    for (auto& article : data.articles) {
        auto& name = supplier ? article.supplier : article.category;
        if (name == oldName) name = group.name;
    }
    if (!supplier)
        for (auto& sector : data.sectors)
            std::ranges::replace(sector.categories, oldName, group.name);
}

void editSector(Inventory& data, int row) {
    Sector sector;
    if (row >= 0) sector = data.sectors.at(row);
    Form form{L"Sector details", {{L"Name", sector.name},
        {L"Employees (separate with ;)", join(sector.employees)},
        {L"Categories (separate with ;)", join(sector.categories)}}};
    if (!prompt(form)) return;
    sector.name = form.fields[0].value;
    if (sector.name.empty()) fail("Enter a sector name.");
    sector.employees = split(form.fields[1].value);
    sector.categories = split(form.fields[2].value);
    for (const auto& category : sector.categories)
        if (std::ranges::find(data.categories, category, &Group::name) == data.categories.end())
            fail("Create the listed categories in the Categories tab first.");
    for (int i = 0; i < static_cast<int>(data.sectors.size()); ++i) {
        if (i == row) continue;
        const auto& other = data.sectors[i];
        if (other.name == sector.name) fail("This sector already exists.");
        for (const auto& name : sector.categories)
            if (std::ranges::find(other.categories, name) != other.categories.end())
                fail("A category is already assigned to another sector.");
        for (const auto& name : sector.employees)
            if (std::ranges::find(other.employees, name) != other.employees.end())
                fail("An employee is already assigned to another sector.");
    }
    if (row < 0) data.sectors.push_back(sector);
    else data.sectors[row] = sector;
}

Article& basketArticle(Inventory& data, int barcode) {
    auto item = std::ranges::find(data.articles, barcode, &Article::barcode);
    if (item == data.articles.end()) fail("Barcode was not found.");
    return *item;
}

void addToBasket() {
    Form form{L"Add to receipt", {{L"Barcode", L""}, {L"Quantity", L"1"}}};
    if (!prompt(form)) return;
    const auto barcode = integer(form.fields[0].value, 1);
    const auto quantity = integer(form.fields[1].value, 1);
    const auto& item = basketArticle(inventory, barcode);
    auto line = std::ranges::find(basket, barcode, &Sale::receipt);
    const int reserved = line == basket.end() ? 0 : line->quantity;
    if (quantity > item.stock - reserved) fail("There is not enough stock for this sale.");
    if (line == basket.end()) basket.push_back({barcode, quantity, item.name, inventory.price(item)});
    else line->quantity += quantity;
}

void saveReceipt(int number, const Inventory& data, bool newSale) {
    const auto path = folder / L"Receipts" / (L"Receipt-" + std::to_wstring(number) + L".txt");
    replaceFile(path,
        [&](auto& output) {
            output << L"Receipt " << number << L'\n';
            if (newSale) {
                SYSTEMTIME date{};
                GetLocalTime(&date);
                output << L"Date: " << date.wYear << L'-'
                       << std::setfill(L'0') << std::setw(2) << date.wMonth << L'-'
                       << std::setw(2) << date.wDay << L'\n';
            }
            output << L'\n';
            double total = 0;
            for (const auto& line : data.sales) {
                if (line.receipt != number) continue;
                const auto amount = line.price * line.quantity;
                total += amount;
                output << line.name << L"   " << line.quantity << L" x " << money(line.price)
                       << L"   " << money(amount) << L'\n';
            }
            output << L"\nTotal: " << money(total) << L'\n';
        });
}

void commit(Inventory next) {
    saveInventory(folder / L"inventory.txt", next);
    inventory = std::move(next);
}

void finishSale() {
    if (basket.empty()) fail("Add an article to the receipt first.");
    Inventory next = inventory;
    int number = 0;
    for (const auto& line : next.sales) number = std::max(number, line.receipt);
    if (number == std::numeric_limits<int>::max()) fail("No receipt numbers remain.");
    ++number;
    // Validate all lines before saving the stock and receipt history together.
    for (const auto& line : basket) {
        auto& item = basketArticle(next, line.receipt);
        if (line.quantity > item.stock || item.sold > std::numeric_limits<int>::max() - line.quantity)
            fail("The available stock changed. Review the receipt.");
        item.stock -= line.quantity;
        item.sold += line.quantity;
        next.sales.push_back({number, line.quantity, line.name, line.price});
    }
    commit(std::move(next));
    basket.clear();
    try {
        saveReceipt(number, inventory, true);
    } catch (const std::exception&) {
        fail("Sale saved. The receipt file could not be written. Export it from Receipts.");
    }
    MessageBoxW(mainWindow, L"Sale saved. The text receipt is in the Receipts folder.",
                L"Guitaar", MB_OK | MB_ICONINFORMATION);
}

void columns(std::initializer_list<const wchar_t*> names) {
    while (ListView_DeleteColumn(table, 0)) {}
    int i = 0;
    for (auto name : names) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(name);
        column.cx = i == 0 ? 160 : 115;
        ListView_InsertColumn(table, i++, &column);
    }
}

void append(std::initializer_list<Text> values) {
    const int row = ListView_GetItemCount(table);
    LVITEMW entry{};
    entry.mask = LVIF_TEXT;
    entry.iItem = row;
    entry.pszText = const_cast<wchar_t*>(values.begin()->c_str());
    ListView_InsertItem(table, &entry);
    int col = 0;
    for (const auto& value : values) {
        if (col) ListView_SetItemText(table, row, col, const_cast<wchar_t*>(value.c_str()));
        ++col;
    }
}

void refresh() {
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    Text summary;
    if (page == Articles) {
        columns({L"Article", L"Barcode", L"Stock", L"Selling price", L"Purchase price",
            L"Category", L"Supplier", L"Type", L"Notes", L"Sold", L"Profit estimate",
            L"Default markup %", L"Custom markup %", L"Discount %", L"Total discount %"});
        for (const auto& item : inventory.articles) {
            const auto price = inventory.price(item);
            append({item.name, std::to_wstring(item.barcode), std::to_wstring(item.stock),
                money(price), money(item.cost), item.category, item.supplier, item.type, item.notes,
                std::to_wstring(item.sold), money((price - item.cost) * item.sold),
                money(inventory.margin(item)), money(item.margin), money(item.discount),
                money(inventory.discount(item))});
        }
    } else if (page == Categories || page == Suppliers) {
        const bool supplier = page == Suppliers;
        columns({L"Name", L"Markup %", L"Discount %", L"Articles", L"Contact"});
        for (const auto& group : supplier ? inventory.suppliers : inventory.categories) {
            const auto count = std::ranges::count(inventory.articles, group.name,
                supplier ? &Article::supplier : &Article::category);
            append({group.name, money(group.margin), money(group.discount),
                    std::to_wstring(count), group.contact});
        }
    } else if (page == Sectors) {
        columns({L"Sector", L"Employees", L"Categories"});
        for (const auto& sector : inventory.sectors)
            append({sector.name, join(sector.employees), join(sector.categories)});
    } else {
        const bool checkout = page == Checkout;
        columns({L"Article", checkout ? L"Barcode" : L"Receipt", L"Quantity", L"Unit price", L"Total"});
        double total = 0;
        for (const auto& line : checkout ? basket : inventory.sales) {
            total += line.quantity * line.price;
            append({line.name, std::to_wstring(line.receipt), std::to_wstring(line.quantity),
                    money(line.price), money(line.quantity * line.price)});
        }
        if (checkout) summary = L"Receipt total: " + money(total);
    }
    if (summary.empty()) summary = std::to_wstring(ListView_GetItemCount(table)) + L" entries";
    SetWindowTextW(status, summary.c_str());
    const bool checkout = page == Checkout;
    const bool history = page == History;
    ShowWindow(GetDlgItem(mainWindow, Add), history ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(mainWindow, Delete), history ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(mainWindow, Edit), checkout || history ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(mainWindow, Finish), checkout || history ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(mainWindow, CancelSale), checkout ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(mainWindow, DefaultPrices), page == Articles ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(mainWindow, CustomPrices), page == Articles ? SW_SHOW : SW_HIDE);
    SetWindowTextW(GetDlgItem(mainWindow, Finish), history ? L"Export receipt" : L"Complete sale");
    SetWindowTextW(GetDlgItem(mainWindow, Delete), checkout ? L"Remove line" : L"Delete");
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, nullptr, TRUE);
}

void deleteRow(Inventory& data, int row) {
    if (page == Articles) data.articles.erase(data.articles.begin() + row);
    if (page == Sectors) data.sectors.erase(data.sectors.begin() + row);
    if (page == Categories || page == Suppliers) {
        auto& groups = page == Categories ? data.categories : data.suppliers;
        const auto name = groups.at(row).name;
        for (auto& item : data.articles) {
            auto& link = page == Categories ? item.category : item.supplier;
            if (link == name) link.clear();
        }
        if (page == Categories)
            for (auto& sector : data.sectors) std::erase(sector.categories, name);
        groups.erase(groups.begin() + row);
    }
}

void command(int id) {
    const auto row = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    if (id == OpenFolder) {
        fs::create_directories(folder);
        ShellExecuteW(mainWindow, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == Import) {
        if (!basket.empty()) fail("Complete or clear the current sale before importing.");
        wchar_t path[32768]{};
        OPENFILENAMEW file{};
        file.lStructSize = sizeof(file);
        file.hwndOwner = mainWindow;
        file.lpstrFilter = L"Java inventory (*.dat)\0*.dat\0All files\0*.*\0";
        file.lpstrFile = path;
        file.nMaxFile = 32768;
        file.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&file) && MessageBoxW(mainWindow,
            L"Replace this app's inventory with the selected Java inventory?",
            L"Import Java inventory", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES)
            commit(importJava(path));
        return;
    }
    if (page == History) {
        if (id == Finish && row >= 0) {
            const auto number = inventory.sales.at(row).receipt;
            const auto path = folder / L"Receipts" / (L"Receipt-" + std::to_wstring(number) + L".txt");
            if (!fs::exists(path)) saveReceipt(number, inventory, false);
            ShellExecuteW(mainWindow, L"open", (folder / L"Receipts").c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    if (page == Checkout) {
        if (id == Add) addToBasket();
        if (id == Delete && row >= 0) basket.erase(basket.begin() + row);
        if (id == CancelSale) basket.clear();
        if (id == Finish) finishSale();
        return;
    }
    if (id != Add && row < 0) fail("Select an entry first.");
    if (page == Articles && !basket.empty() && (id == Delete || id == Edit))
        fail("Complete or clear the current sale before changing articles.");
    Inventory next = inventory;
    if (id == Delete) {
        if (MessageBoxW(mainWindow, L"Delete the selected entry?", L"Guitaar",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
        deleteRow(next, row);
    } else if (id == DefaultPrices || id == CustomPrices) {
        next.articles.at(row).pricing = id == DefaultPrices ? 0 : 1;
    } else {
        const int target = id == Add ? -1 : row;
        if (page == Articles) editArticle(next, target);
        if (page == Categories || page == Suppliers) editGroup(next, target, page == Suppliers);
        if (page == Sectors) editSector(next, target);
    }
    commit(std::move(next));
}

LRESULT CALLBACK mainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        mainWindow = window;
        tabs = control(window, WC_TABCONTROLW, L"", WS_TABSTOP, 12, 12, 650, 28);
        for (auto name : {L"Articles", L"Categories", L"Suppliers", L"Sectors", L"Checkout", L"Receipts"}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(name);
            TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item);
        }
        table = control(window, WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT |
            LVS_SHOWSELALWAYS | LVS_SINGLESEL, 12, 90, 900, 400);
        ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        control(window, L"BUTTON", L"Add", WS_TABSTOP, 12, 48, 85, 30, Add);
        control(window, L"BUTTON", L"Edit", WS_TABSTOP, 103, 48, 85, 30, Edit);
        control(window, L"BUTTON", L"Delete", WS_TABSTOP, 194, 48, 100, 30, Delete);
        control(window, L"BUTTON", L"Complete sale", WS_TABSTOP, 301, 48, 130, 30, Finish);
        control(window, L"BUTTON", L"Clear receipt", WS_TABSTOP, 437, 48, 110, 30, CancelSale);
        control(window, L"BUTTON", L"Automatic price", WS_TABSTOP, 301, 48, 130, 30, DefaultPrices);
        control(window, L"BUTTON", L"Custom margin", WS_TABSTOP, 437, 48, 115, 30, CustomPrices);
        status = control(window, L"STATIC", L"", 0, 12, 512, 450, 24);
        HMENU menu = CreateMenu();
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, Import, L"Import Java inventory...");
        AppendMenuW(file, MF_STRING, OpenFolder, L"Open data folder");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
        SetMenu(window, menu);
        refresh();
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {590, 365};
        return 0;
    case WM_SIZE:
        MoveWindow(tabs, 12, 12, LOWORD(lParam) - 24, 28, TRUE);
        MoveWindow(table, 12, 88, LOWORD(lParam) - 24, HIWORD(lParam) - 125, TRUE);
        MoveWindow(status, 13, HIWORD(lParam) - 28, LOWORD(lParam) - 26, 21, TRUE);
        return 0;
    case WM_NOTIFY: {
        const auto* info = reinterpret_cast<NMHDR*>(lParam);
        if (info->hwndFrom == tabs && info->code == TCN_SELCHANGE) {
            page = static_cast<Page>(TabCtrl_GetCurSel(tabs));
            refresh();
        }
        if (info->hwndFrom == table && info->code == NM_DBLCLK &&
            ListView_GetNextItem(table, -1, LVNI_SELECTED) >= 0 && page != Checkout)
            SendMessageW(window, WM_COMMAND, page == History ? Finish : Edit, 0);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) < Add || LOWORD(wParam) > CustomPrices) break;
        try {
            command(LOWORD(wParam));
        } catch (const std::invalid_argument&) {
            MessageBoxW(window, L"Enter a valid number in each numeric field.", L"Guitaar", MB_OK | MB_ICONWARNING);
        } catch (const std::exception& error) {
            MessageBoxA(window, error.what(), "Guitaar", MB_OK | MB_ICONWARNING);
        }
        refresh();
        return 0;
    case WM_CLOSE:
        if (!basket.empty() && MessageBoxW(window, L"Close and discard the unfinished receipt?",
            L"Guitaar", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int show) {
    // A single editor prevents two windows from overwriting each other's inventory.
    const HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\GuitaarInventory");
    if (!lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Guitaar is already open.", L"Guitaar", MB_OK);
        if (lock) CloseHandle(lock);
        return 1;
    }
    try {
        PWSTR localData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localData)))
            fail("Could not locate the Windows application data folder.");
        folder = fs::path(localData) / L"Guitaar";
        CoTaskMemFree(localData);
        inventory = loadInventory(folder / L"inventory.txt");
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Guitaar", MB_OK | MB_ICONERROR);
        CloseHandle(lock);
        return 1;
    }
    instance = app;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&common);
    font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = formProc;
    wc.lpszClassName = L"GuitaarForm";
    RegisterClassW(&wc);
    wc.lpfnWndProc = mainProc;
    wc.lpszClassName = L"GuitaarMain";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Guitaar - Inventory", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680, nullptr, nullptr, instance, nullptr);
    if (!window) {
        CloseHandle(lock);
        return 1;
    }
    ShowWindow(window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DeleteObject(font);
    CloseHandle(lock);
    return static_cast<int>(message.wParam);
}
