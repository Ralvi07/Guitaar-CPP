// Serializes the native inventory to disk and reconstructs its records when loading.
#pragma once
#include "Inventory.hpp"

#include <windows.h>
#include <codecvt>
#include <iomanip>
#include <locale>

inline std::locale utf8() {
    return std::locale(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>);
}

template<class Stream, class Row, class Fields>
void writeRows(Stream& stream, const std::vector<Row>& rows, Fields fields) {
    stream << rows.size() << L'\n';
    for (const auto& row : rows) {
        fields(stream, row);
        stream << L'\n';
    }
}

template<class Stream, class Row, class Fields>
void readRows(Stream& stream, std::vector<Row>& rows, Fields fields) {
    int count = 0;
    if (!(stream >> count) || count < 0 || count > 1000000)
        throw std::runtime_error("Invalid Guitaar inventory file.");
    while (count-- > 0) {
        Row row{};
        fields(stream, row);
        if (!stream) throw std::runtime_error("Incomplete Guitaar inventory file.");
        rows.push_back(std::move(row));
    }
}

// Read the saved rows into the corresponding inventory collections.
inline Inventory loadInventory(const fs::path& path) {
    Inventory data;
    if (!fs::exists(path)) return data;
    std::wifstream input(path, std::ios::binary);
    input.imbue(utf8());
    Text version;
    if (!(input >> version) || version != L"GUITAAR1")
        throw std::runtime_error("Could not read the Guitaar inventory.");
    readRows(input, data.articles, [](auto& in, Article& a) {
        in >> a.barcode >> a.stock >> a.sold >> a.cost >> a.price >> a.margin
           >> a.discount >> a.pricing >> std::quoted(a.name) >> std::quoted(a.type)
           >> std::quoted(a.notes) >> std::quoted(a.category) >> std::quoted(a.supplier);
    });
    auto readGroup = [](auto& in, Group& g) {
        in >> std::quoted(g.name) >> std::quoted(g.contact) >> g.margin >> g.discount;
    };
    readRows(input, data.categories, readGroup);
    readRows(input, data.suppliers, readGroup);
    readRows(input, data.sectors, [](auto& in, Sector& s) {
        in >> std::quoted(s.name);
        auto readName = [](auto& stream, Text& name) { stream >> std::quoted(name); };
        readRows(in, s.employees, readName);
        readRows(in, s.categories, readName);
    });
    readRows(input, data.sales, [](auto& in, Sale& s) {
        in >> s.receipt >> s.quantity >> s.price >> std::quoted(s.name);
    });
    return data;
}

// Write the replacement separately so a failed save can leave the existing file intact.
inline void replaceFile(const fs::path& path, auto write) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    std::wofstream output(temporary, std::ios::binary);
    output.imbue(utf8());
    write(output);
    output.close();
    // Keep the previous saved file if writing the replacement fails.
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not save the file.");
}

inline void saveInventory(const fs::path& path, const Inventory& data) {
    replaceFile(path, [&](auto& output) {
        output << L"GUITAAR1\n" << std::setprecision(17);
        writeRows(output, data.articles, [](auto& out, const Article& a) {
            out << a.barcode << L' ' << a.stock << L' ' << a.sold << L' ' << a.cost
                << L' ' << a.price << L' ' << a.margin << L' ' << a.discount
                << L' ' << a.pricing << L' ' << std::quoted(a.name) << L' '
                << std::quoted(a.type) << L' ' << std::quoted(a.notes) << L' '
                << std::quoted(a.category) << L' ' << std::quoted(a.supplier);
        });
        auto writeGroup = [](auto& out, const Group& g) {
            out << std::quoted(g.name) << L' ' << std::quoted(g.contact)
                << L' ' << g.margin << L' ' << g.discount;
        };
        writeRows(output, data.categories, writeGroup);
        writeRows(output, data.suppliers, writeGroup);
        writeRows(output, data.sectors, [](auto& out, const Sector& s) {
            out << std::quoted(s.name) << L'\n';
            auto writeName = [](auto& stream, const Text& name) { stream << std::quoted(name); };
            writeRows(out, s.employees, writeName);
            writeRows(out, s.categories, writeName);
        });
        writeRows(output, data.sales, [](auto& out, const Sale& s) {
            out << s.receipt << L' ' << s.quantity << L' ' << s.price << L' ' << std::quoted(s.name);
        });
    });
}
