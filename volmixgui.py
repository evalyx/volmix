import sys, subprocess, re, os
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                             QHBoxLayout, QComboBox, QLabel, QPushButton,
                             QTableWidget, QTableWidgetItem, QHeaderView,
                             QAbstractItemView, QFileDialog, QSpinBox)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QFont

CONFIG_FILE = "volmix.conf"

BREEZE_STYLE = """
    QMainWindow { background-color: #31363b; }
    QLabel { color: #eff0f1; font-family: 'Noto Sans', sans-serif; }
    QSpinBox, QComboBox {
        background-color: #232629; color: #eff0f1; border: 1px solid #76797c;
        padding: 4px; border-radius: 2px;
    }
    QPushButton {
        background-color: #4d5057; color: #eff0f1; border: 1px solid #76797c;
        padding: 6px 15px; border-radius: 3px;
    }
    QPushButton:hover { background-color: #3daee9; border: 1px solid #3daee9; }
    QTableWidget {
        background-color: #232629; gridline-color: #4d5057; color: #eff0f1;
        border: 1px solid #76797c;
    }
    QHeaderView::section {
        background-color: #31363b; color: #eff0f1; padding: 8px;
        border: 1px solid #4d5057; font-weight: bold;
    }
"""

class VolMixMatrix(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("VolMix - Matrix")
        self.setMinimumSize(1000, 550)
        self.setStyleSheet(BREEZE_STYLE)

        self.all_targets = {}
        self.visible_columns = []
        self.mappings = {} # Store as {(layer, fader): [target_id1, target_id2]}

        self.init_ui()
        self.refresh_wpctl()
        self.load_config(CONFIG_FILE)
        self.rebuild_grid()

    def init_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)

        toolbar = QHBoxLayout()
        toolbar.addWidget(QLabel("<b>LAYER:</b>"))
        self.layer_spin = QSpinBox()
        self.layer_spin.setRange(0, 99)
        self.layer_spin.valueChanged.connect(self.rebuild_grid)
        toolbar.addWidget(self.layer_spin)

        toolbar.addSpacing(30)
        self.target_picker = QComboBox()
        toolbar.addWidget(self.target_picker, 1)

        for text, func in [("+ Add Col", self.add_column), ("- Rem Col", self.remove_column), ("↻ Refresh", self.refresh_wpctl)]:
            btn = QPushButton(text)
            btn.clicked.connect(func)
            toolbar.addWidget(btn)
        layout.addLayout(toolbar)

        self.table = QTableWidget()
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.cellClicked.connect(self.handle_cell_click)
        layout.addWidget(self.table)

        btns = QHBoxLayout()
        for t, f in [("Import", self.import_dialog), ("Export", self.export_dialog)]:
            b = QPushButton(t); b.clicked.connect(f); btns.addWidget(b)
        btns.addStretch()
        btns.addWidget(QLabel("<small>Sylveon VolMix | Multiple Targets per Fader Enabled</small>"))
        layout.addLayout(btns)

    def refresh_wpctl(self):
        self.target_picker.clear()
        try:
            output = subprocess.check_output("wpctl status", shell=True).decode()
            sect = ""
            for line in output.splitlines():
                if "Sinks:" in line: sect = "OUT"
                elif "Sources:" in line: sect = "IN"
                elif "Streams:" in line: sect = "APP"
                match = re.search(r"(?:[*|]\s+)?(\d+)\.\s+(.*)", line)
                if match and sect:
                    id_n, name = match.group(1), match.group(2).strip().split('[')[0].strip()
                    self.all_targets[id_n] = f"[{sect}] {name}"
                    self.target_picker.addItem(f"[{sect}] {name}", id_n)
        except: pass

    def add_column(self):
        tid = self.target_picker.currentData()
        if tid and tid not in self.visible_columns:
            self.visible_columns.append(tid); self.rebuild_grid()

    def remove_column(self):
        if self.table.currentColumn() >= 0:
            self.visible_columns.pop(self.table.currentColumn()); self.rebuild_grid()

    def handle_cell_click(self, row, col):
        layer, fader, target_id = self.layer_spin.value(), row + 1, self.visible_columns[col]

        # 1. ENFORCE UNIQUE TARGET: If this target is bound anywhere else in this layer, remove it first
        for f in range(1, 8):
            if f != fader and target_id in self.mappings.get((layer, f), []):
                self.mappings[(layer, f)].remove(target_id)

        # 2. TOGGLE BINDING:
        current_list = self.mappings.get((layer, fader), [])
        if target_id in current_list:
            current_list.remove(target_id)
        else:
            current_list.append(target_id)

        self.mappings[(layer, fader)] = current_list
        self.rebuild_grid()
        self.save_config(CONFIG_FILE)

    def rebuild_grid(self):
        layer = self.layer_spin.value()
        self.table.setRowCount(7)
        self.table.setColumnCount(len(self.visible_columns))
        self.table.setVerticalHeaderLabels([f"FADER {i+1}" for i in range(7)])
        self.table.setHorizontalHeaderLabels([self.all_targets.get(t, t) for t in self.visible_columns])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)

        for row in range(7):
            for col, tid in enumerate(self.visible_columns):
                active = tid in self.mappings.get((layer, row+1), [])
                item = QTableWidgetItem("BOUND" if active else "-")
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                if active:
                    item.setBackground(QColor("#3daee9"))
                    item.setForeground(QColor("#ffffff"))
                self.table.setItem(row, col, item)

    def save_config(self, filepath):
        with open(filepath, "w") as f:
            for (lay, fad), targets in self.mappings.items():
                for tid in targets:
                    name = re.sub(r'\[.*?\]\s*', '', self.all_targets.get(tid, "Unk"))[:10].replace(" ", "_")
                    f.write(f"{lay} {fad} {tid} {name}\n")

    def load_config(self, filepath):
        if not os.path.exists(filepath): return
        self.mappings = {}
        with open(filepath, "r") as f:
            for line in f:
                p = line.strip().split()
                if len(p) >= 3:
                    l, fad, tid = int(p[0]), int(p[1]), p[2]
                    if (l, fad) not in self.mappings: self.mappings[(l, fad)] = []
                    if tid not in self.mappings[(l, fad)]: self.mappings[(l, fad)].append(tid)
                    if tid not in self.visible_columns: self.visible_columns.append(tid)

    def export_dialog(self):
        p, _ = QFileDialog.getSaveFileName(self, "Export", "", "Conf (*.conf)")
        if p: self.save_config(p)

    def import_dialog(self):
        p, _ = QFileDialog.getOpenFileName(self, "Import", "", "Conf (*.conf)")
        if p: self.load_config(p); self.save_config(CONFIG_FILE); self.rebuild_grid()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    win = VolMixMatrix(); win.show()
    sys.exit(app.exec())
