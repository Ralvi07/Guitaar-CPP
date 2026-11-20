// Defines the inventory records and operations shared by the native application's interface and storage code.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Text = std::wstring;
namespace fs = std::filesystem;

struct Article {
    int barcode = 0, stock = 0, sold = 0;
    Text name, type, notes, category, supplier;
    double cost = 0, price = 0, margin = 0, discount = 0;
    int pricing = 0; // 0 = automatic, 1 = custom margin, 2 = fixed price
};

struct Group {
    Text name, contact;
    double margin = 0, discount = 0;
};

struct Sector {
    Text name;
    std::vector<Text> employees, categories;
};

struct Sale {
    int receipt = 0, quantity = 0;
    Text name;
    double price = 0;
};

struct Inventory {
    std::vector<Article> articles;
    std::vector<Group> categories, suppliers;
    std::vector<Sector> sectors;
    std::vector<Sale> sales;

    static const Group& group(const std::vector<Group>& rows, const Text& name) {
        static const Group empty;
        auto found = std::ranges::find(rows, name, &Group::name);
        return found == rows.end() ? empty : *found;
    }

    double margin(const Article& item) const {
        return group(categories, item.category).margin + group(suppliers, item.supplier).margin;
    }

    double discount(const Article& item) const {
        return item.discount + group(categories, item.category).discount
            + group(suppliers, item.supplier).discount;
    }

    double price(const Article& item) const {
        if (item.pricing == 2) return item.price;
        const auto markup = item.pricing == 1 ? item.margin : margin(item);
        return std::round(item.cost * (1 + markup / 100)
            * std::max(0.0, 1 - discount(item) / 100) * 100) / 100;
    }
};

// Java DataInputStream uses big-endian numbers and modified UTF-8.
class LegacyReader {
    std::ifstream input;

    unsigned byte() {
        const int value = input.get();
        if (value == EOF) throw std::runtime_error("The Java inventory file is incomplete.");
        return static_cast<unsigned>(value);
    }

    std::uint64_t number(int bytes) {
        std::uint64_t value = 0;
        while (bytes-- > 0) value = (value << 8) | byte();
        return value;
    }

public:
    explicit LegacyReader(const fs::path& path) : input(path, std::ios::binary) {
        if (!input) throw std::runtime_error("Could not open the Java inventory.");
    }

    int integer() { return static_cast<std::int32_t>(number(4)); }
    double decimal() {
        const auto value = std::bit_cast<double>(number(8));
        if (!std::isfinite(value)) throw std::runtime_error("Invalid amount in Java inventory.");
        return value;
    }

    int count() {
        const auto value = integer();
        if (value < 0 || value > 1000000) throw std::runtime_error("Invalid Java inventory file.");
        return value;
    }

    Text string() {
        int bytes = static_cast<int>(number(2));
        Text result;
        while (bytes > 0) {
            auto value = byte();
            --bytes;
            int extra = value < 0x80 ? 0 : value < 0xE0 && value >= 0xC0 ? 1
                : value >= 0xE0 && value < 0xF0 ? 2 : -1;
            if (extra < 0 || extra > bytes) throw std::runtime_error("Invalid Java text.");
            if (extra) value &= (1u << (6 - extra)) - 1;
            bytes -= extra;
            while (extra-- > 0) {
                const auto next = byte();
                if ((next & 0xC0) != 0x80) throw std::runtime_error("Invalid Java text.");
                value = (value << 6) | (next & 0x3F);
            }
            result += static_cast<wchar_t>(value);
        }
        return result;
    }

    std::vector<Text> names() {
        std::vector<Text> result;
        for (int size = count(); size > 0; --size) result.push_back(string());
        return result;
    }
};

// Read the legacy Java storage format into the native inventory model.
inline Inventory importJava(const fs::path& path) {
    LegacyReader in(path);
    Inventory data;
    for (int count = in.count(); count > 0; --count) {
        Article item;
        item.barcode = in.integer();
        item.name = in.string();
        item.notes = in.string();
        item.cost = in.decimal();
        item.price = in.decimal();
        item.stock = in.integer();
        item.margin = in.decimal();
        item.type = in.string();
        item.sold = in.integer();
        in.decimal(); // Profit is calculated from the current price.
        item.category = in.string();
        in.decimal();
        item.supplier = in.string();
        in.string();
        in.decimal();
        in.decimal();
        item.discount = in.decimal();
        in.decimal();
        in.decimal();
        if (item.category == L"NULL") item.category.clear();
        if (item.supplier == L"NULL") item.supplier.clear();
        item.pricing = 2; // Preserve the saved selling price when importing.
        data.articles.push_back(item);
    }

    for (bool supplier : {false, true}) {
        auto& groups = supplier ? data.suppliers : data.categories;
        for (int count = in.count(); count > 0; --count) {
            Group group;
            in.integer();
            if (supplier) {
                group.name = in.string();
                in.integer();
                group.contact = in.string();
            } else {
                in.integer();
                group.name = in.string();
            }
            group.margin = in.decimal();
            for (int items = in.count(); items > 0; --items) in.integer();
            group.discount = in.decimal();
            groups.push_back(group);
        }
    }
    for (int count = in.count(); count > 0; --count) {
        auto name = in.string();
        auto employees = in.names();
        data.sectors.push_back({name, employees, in.names()});
    }
    for (int count = in.count(); count > 0; --count) {
        Sale sale;
        sale.receipt = in.integer();
        in.integer(); // The second number is Java's shared receipt counter.
        sale.name = in.string();
        sale.price = in.decimal();
        sale.quantity = in.integer();
        data.sales.push_back(sale);
    }
    return data;
}
