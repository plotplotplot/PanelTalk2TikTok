import sys, os, math, time
import numpy as np
import av

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QSlider,
    QLabel, QFileDialog, QListWidget, QListWidgetItem,
    QMessageBox, QSplitter, QPushButton, QHBoxLayout
)
from PySide6.QtGui import QAction, QSurfaceFormat
from PySide6.QtOpenGLWidgets import QOpenGLWidget

from OpenGL.GL import *
from OpenGL.GL.shaders import compileProgram, compileShader


LAST_DIR_PATH = "last_dir.txt"


# -----------------------------------------------------------
# SHADERS
# -----------------------------------------------------------
VERT_SHADER = """
#version 330 core
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

uniform mat4 uTransform;
out vec2 vUV;

void main() {
    gl_Position = uTransform * vec4(inPos, 0.0, 1.0);
    vUV = inUV;
}
"""

FRAG_SHADER = """
#version 330 core
in vec2 vUV;
out vec4 outColor;

uniform sampler2D uTex;

void main() {
    outColor = texture(uTex, vUV);
}
"""


# -----------------------------------------------------------
# OPENGL VIDEO WIDGET
# -----------------------------------------------------------
class GLVideoWidget(QOpenGLWidget):
    def __init__(self):
        super().__init__()

        self.setUpdateBehavior(QOpenGLWidget.PartialUpdate)

        self.angle = 0.0
        self.zoom = 1.0
        self.offset_x = 0.0
        self.offset_y = 0.0

        self.container = None
        self.stream = None
        self.frame_iter = None

        self._initialized = False
        self.timer = None

        self.playing = False
        self.current_pts = 0
        self.duration = 0
        self.fps = 30
        self.last_time = 0

    # -------------------------------------------------------
    def initializeGL(self):
        super().initializeGL()

        glEnable(GL_TEXTURE_2D)

        self.program = compileProgram(
            compileShader(VERT_SHADER, GL_VERTEX_SHADER),
            compileShader(FRAG_SHADER, GL_FRAGMENT_SHADER)
        )

        self.vertexData = np.array([
            -1, -1, 0, 0,
             1, -1, 1, 0,
             1,  1, 1, 1,
            -1,  1, 0, 1,
        ], dtype=np.float32)

        self.indices = np.array([0, 1, 2, 2, 3, 0], dtype=np.uint32)

        self.vbo = glGenBuffers(1)
        glBindBuffer(GL_ARRAY_BUFFER, self.vbo)
        glBufferData(GL_ARRAY_BUFFER, self.vertexData.nbytes, self.vertexData, GL_STATIC_DRAW)

        self.ebo = glGenBuffers(1)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, self.ebo)
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, self.indices.nbytes, self.indices, GL_STATIC_DRAW)

        self.texture = glGenTextures(1)
        glBindTexture(GL_TEXTURE_2D, self.texture)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)

        self._initialized = True

        self.timer = QTimer()
        self.timer.timeout.connect(self.next_frame)
        self.timer.start(5)

    # -------------------------------------------------------
    def paintGL(self):
        if not self._initialized:
            return

        glClearColor(0.1, 0.1, 0.1, 1)
        glClear(GL_COLOR_BUFFER_BIT)

        glUseProgram(self.program)

        transform = self.build_matrix()
        loc = glGetUniformLocation(self.program, "uTransform")
        glUniformMatrix4fv(loc, 1, GL_FALSE, transform)

        glActiveTexture(GL_TEXTURE0)
        glBindTexture(GL_TEXTURE_2D, self.texture)
        glUniform1i(glGetUniformLocation(self.program, "uTex"), 0)

        glBindBuffer(GL_ARRAY_BUFFER, self.vbo)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, self.ebo)

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(0))
        glEnableVertexAttribArray(0)

        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(8))
        glEnableVertexAttribArray(1)

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, None)

    # -------------------------------------------------------
    def load_video(self, path):
        try:
            self.container = av.open(path)

            # ---- PICK FIRST VALID VIDEO STREAM ----
            video_streams = [s for s in self.container.streams if s.type == "video"]
            if not video_streams:
                raise Exception("No video stream found.")

            self.stream = video_streams[0]

            self.frame_iter = self.container.decode(self.stream)

            self.duration = self.stream.duration * self.stream.time_base if self.stream.duration else 0
            self.fps = float(self.stream.average_rate) if self.stream.average_rate else 30

            self.current_pts = 0
            self.last_time = time.time()

            self.playing = True

        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to load video:\n{e}")

    # -------------------------------------------------------
    def seek(self, position_seconds):
        if not self.container or not self.stream:
            return

        try:
            self.container.seek(int(position_seconds / self.stream.time_base))
            self.frame_iter = self.container.decode(self.stream)
            self.current_pts = position_seconds
        except Exception as e:
            print("Seek error:", e)

    # -------------------------------------------------------
    def next_frame(self):
        if not self.playing or not self._initialized:
            return
        if not self.frame_iter:
            return

        now = time.time()
        if now - self.last_time < 1.0 / self.fps:
            return
        self.last_time = now

        try:
            frame = next(self.frame_iter)
        except StopIteration:
            self.playing = False
            return

        self.current_pts = float(frame.pts * self.stream.time_base)

        rgb = frame.to_ndarray(format="rgb24")

        glBindTexture(GL_TEXTURE_2D, self.texture)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     frame.width, frame.height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, rgb)

        self.update()

    # -------------------------------------------------------
    def build_matrix(self):
        c = math.cos(self.angle)
        s = math.sin(self.angle)

        mat = np.array([
            [self.zoom*c, -self.zoom*s, 0, 0],
            [self.zoom*s,  self.zoom*c, 0, 0],
            [0, 0, 1, 0],
            [self.offset_x, self.offset_y, 0, 1],
        ], dtype=np.float32)

        return mat.flatten()

    def set_angle(self, v): self.angle = math.radians(v); self.update()
    def set_zoom(self, v): self.zoom = v / 100.0; self.update()
    def set_offset_x(self, v): self.offset_x = (v - 50) / 50.0; self.update()
    def set_offset_y(self, v): self.offset_y = (v - 50) / 50.0; self.update()


# -----------------------------------------------------------
# MAIN WINDOW
# -----------------------------------------------------------
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle("Video Editor - OpenGL Player")

        # Menu
        menubar = self.menuBar()
        file_menu = menubar.addMenu("File")

        open_dir_action = QAction("Open Directory…", self)
        open_dir_action.triggered.connect(self.open_directory)
        file_menu.addAction(open_dir_action)

        # Splitter layout
        splitter = QSplitter(Qt.Horizontal)
        splitter.setChildrenCollapsible(False)

        self.file_list = QListWidget()
        self.file_list.itemClicked.connect(self.load_selected_video)

        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)

        self.glwidget = GLVideoWidget()
        right_layout.addWidget(self.glwidget, 5)

        # --- Controls ---
        controls = QHBoxLayout()

        self.play_button = QPushButton("Pause")
        self.play_button.clicked.connect(self.toggle_play)
        controls.addWidget(self.play_button)

        self.seek_slider = QSlider(Qt.Horizontal)
        self.seek_slider.setRange(0, 1000)
        self.seek_slider.sliderPressed.connect(self.pause_for_seek)
        self.seek_slider.sliderReleased.connect(self.seek_video)
        controls.addWidget(self.seek_slider)

        right_layout.addLayout(controls)

        # Transform sliders
        self.add_slider(right_layout, "Rotate", 0, 360, 0, self.glwidget.set_angle)
        self.add_slider(right_layout, "Zoom", 10, 300, 100, self.glwidget.set_zoom)
        self.add_slider(right_layout, "Offset X", 0, 100, 50, self.glwidget.set_offset_x)
        self.add_slider(right_layout, "Offset Y", 0, 100, 50, self.glwidget.set_offset_y)

        splitter.addWidget(self.file_list)
        splitter.addWidget(right_panel)
        splitter.setSizes([200, 1000])

        self.setCentralWidget(splitter)

        # Seek bar updater
        self.seek_timer = QTimer()
        self.seek_timer.timeout.connect(self.update_seek)
        self.seek_timer.start(30)

        # Load last directory
        self.load_last_directory()

    # -------------------------------------------------------
    def save_last_directory(self, path):
        with open(LAST_DIR_PATH, "w") as f:
            f.write(path)

    def load_last_directory(self):
        if not os.path.exists(LAST_DIR_PATH):
            return
        try:
            with open(LAST_DIR_PATH, "r") as f:
                path = f.read().strip()
            if os.path.isdir(path):
                self.populate_directory(path)
        except:
            pass

    # -------------------------------------------------------
    def add_slider(self, layout, name, minv, maxv, start, func):
        label = QLabel(name)
        slider = QSlider(Qt.Horizontal)
        slider.setRange(minv, maxv)
        slider.setValue(start)
        slider.valueChanged.connect(func)
        layout.addWidget(label)
        layout.addWidget(slider)

    # -------------------------------------------------------
    def open_directory(self):
        dir_path = QFileDialog.getExistingDirectory(self, "Select Media Folder")
        if not dir_path:
            return
        self.populate_directory(dir_path)
        self.save_last_directory(dir_path)

    def populate_directory(self, dir_path):
        self.file_list.clear()
        exts = (".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v")
        for name in sorted(os.listdir(dir_path)):
            if name.lower().endswith(exts):
                full_path = os.path.join(dir_path, name)
                self.file_list.addItem(QListWidgetItem(full_path))

    # -------------------------------------------------------
    def load_selected_video(self, item):
        path = item.text()
        self.glwidget.load_video(path)

    # -------------------------------------------------------
    def toggle_play(self):
        self.glwidget.playing = not self.glwidget.playing
        self.play_button.setText("Pause" if self.glwidget.playing else "Play")

    def pause_for_seek(self):
        self.was_playing = self.glwidget.playing
        self.glwidget.playing = False

    def seek_video(self):
        value = self.seek_slider.value() / 1000.0
        target = value * self.glwidget.duration
        self.glwidget.seek(target)
        self.glwidget.playing = self.was_playing

    def update_seek(self):
        if not self.glwidget.stream or not self.glwidget.duration:
            return
        pos = self.glwidget.current_pts / self.glwidget.duration
        pos = max(0, min(pos, 1))
        self.seek_slider.blockSignals(True)
        self.seek_slider.setValue(int(pos * 1000))
        self.seek_slider.blockSignals(False)


# -----------------------------------------------------------
# ENTRY POINT
# -----------------------------------------------------------
if __name__ == "__main__":
    app = QApplication(sys.argv)

    fmt = QSurfaceFormat()
    fmt.setVersion(3, 3)
    fmt.setProfile(QSurfaceFormat.CoreProfile)
    QSurfaceFormat.setDefaultFormat(fmt)

    win = MainWindow()
    win.resize(1400, 900)
    win.show()

    sys.exit(app.exec())
