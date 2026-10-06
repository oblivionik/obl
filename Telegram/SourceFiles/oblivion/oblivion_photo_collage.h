/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_editor.h"

#include <mutex>

// Photo editor: the collage.
//
// A collage is one layer of the document (LayerContent::type() "collage")
// that lays several photos out in a grid: templates generated in code for
// any number of photos, custom rows and columns, splitters dragged on the
// canvas, spacing, an outer margin, rounded corners, a background (none,
// a color, a gradient, a blurred photo) and, for every cell, its own pan
// and zoom, quarter turns and a mirror. Being a layer, it has everything
// layers have: opacity, blending, effects, a mask, a transform.
//
// The model is plain data (CollageData) kept by an immutable
// CollageContent. The grid is a list of tracks (rows, or columns for
// a "columns first" grid) with relative sizes, every track has its own
// list of cells with relative sizes: that covers regular grids as well as
// "one big photo and three small ones". The photos are a separate ordered
// list: the first grid.count() of them are shown, the rest wait (so
// a smaller template never loses a photo, the next bigger one shows it
// again).
//
// Pixels of a photo are shared, never copied: a cell points to
// a CollageSource that wraps the ImageContent of an image layer and keeps
// a pyramid of its halved copies, so a preview of nine 12 MP photos does
// not resample 100 MP on every splitter move. A photo added to a collage
// is kept only as large as the collage can show, see CollagePhotoSize():
// sixteen camera photos take a couple of hundred megabytes, not a gigabyte.
//
// Where the user finds it: the "Collage" tool of the tool strip (its
// options are the whole collage panel), the "Collage" item of the "+"
// menu of the layers panel, the collage section of the Layer tab, and
// ShowCollageEditor() / ChoosePhotosForCollage() for a collage started
// from outside of the editor. While the collage tool is current photos
// dropped on a cell or pasted with Cmd+V go to cells instead of new
// layers.
//
// Everything above the "UI" part of the .cpp is pure and thread safe:
// usable on worker threads, covered by the "collage" sub-test of
// OBLIVION_SELFTEST=photo_doc.
namespace Oblivion::Photo {

inline constexpr auto kCollageMaxTracks = 6; // And cells in one track.
inline constexpr auto kCollageMaxCells = 36;
// The smallest part of a track (or of the collage) a cell can get.
inline constexpr auto kCollageMinWeight = 0.08;
inline constexpr auto kCollageMaxZoom = 8.;
// Parts of the shorter side of the collage.
inline constexpr auto kCollageMaxSpacing = 0.1;
inline constexpr auto kCollageMaxMargin = 0.15;
inline constexpr auto kCollageMaxRadius = 0.5;
// Photos added to a collage are never kept larger than this longer side,
// see CollagePhotoSize() (image layers taken into a collage keep their
// pixels: the undo steps before that hold them anyway).
inline constexpr auto kCollagePhotoMaxSide = 4096;

inline const auto kCollageType = QByteArray("collage");
inline const auto kCollageTool = QByteArray("collage");

//
// The grid.
//

struct CollageGrid {
	// false: the tracks are rows (laid out from the top down, the cells of
	// a row go from the left to the right), true: the tracks are columns.
	bool columns = false;
	std::vector<double> tracks; // Relative sizes, the sum is 1.
	std::vector<std::vector<double>> cells; // The same for every track.

	[[nodiscard]] int count() const; // Cells in all tracks.

	friend bool operator==(
		const CollageGrid &a,
		const CollageGrid &b) = default;
};

// A valid grid made from anything: at least one cell, at most
// kCollageMaxTracks tracks and cells in a track, positive sizes that sum
// to 1 and are not smaller than kCollageMinWeight. A valid grid comes
// back unchanged (bit for bit).
[[nodiscard]] CollageGrid NormalizedCollageGrid(CollageGrid grid);
[[nodiscard]] CollageGrid UniformCollageGrid(int rows, int columns);
// Rows and columns as the "custom grid" sliders show them: the number of
// tracks and the largest number of cells in a track (swapped for
// a columns first grid).
[[nodiscard]] QSize CollageGridSize(const CollageGrid &grid);

struct CollageTemplate {
	QByteArray id; // "r2.3": rows of 2 and 3 cells, "c1.2": columns.
	CollageGrid grid;
};
// The layouts offered for this many photos (1..kCollageMaxCells), the
// most natural one first. Never empty, every grid has exactly count
// cells, no two of them look the same.
[[nodiscard]] std::vector<CollageTemplate> CollageTemplates(int count);
[[nodiscard]] CollageGrid DefaultCollageGrid(int count);
// The same arrangement of cells whatever the sizes are.
[[nodiscard]] bool SameCollageLayout(
	const CollageGrid &a,
	const CollageGrid &b);

// The rectangles of the cells in the pixels of a collage of this size,
// track by track. spacing is the gap between neighbours, margin the gap
// to the edges, both in pixels. With snap everything is whole pixels:
// the margin and every gap are exactly the rounded values, cells never
// overlap and without spacing never leave a hairline between them.
[[nodiscard]] std::vector<QRectF> CollageCellRects(
	const CollageGrid &grid,
	QSizeF size,
	double spacing,
	double margin,
	bool snap = true);

// A border that can be dragged: between two tracks or between two cells
// of one track.
struct CollageSplitter {
	int track = 0;
	int index = -1; // -1: between the tracks track and track + 1.
	bool vertical = false; // A vertical line, dragged horizontally.
	QRectF rect; // The gap itself (no thickness without spacing).
	// The pixels shared by the sizes this splitter changes (the track or
	// the collage without the gaps): a drag by length moves it by 1.
	double length = 0.;
};
[[nodiscard]] std::vector<CollageSplitter> CollageSplitters(
	const CollageGrid &grid,
	QSizeF size,
	double spacing,
	double margin,
	bool snap = true);
// The grid after the splitter was dragged by delta pixels (positive: to
// the right / down). Only the two neighbours change, none of them gets
// smaller than kCollageMinWeight. linked: a splitter between cells moves
// in every track while all tracks are divided the same way (a regular
// grid stays regular).
[[nodiscard]] CollageGrid MoveCollageSplitter(
	const CollageGrid &grid,
	const CollageSplitter &splitter,
	double delta,
	bool linked = true);

//
// A photo in its cell.
//
// zoom 1 is "cover": the photo fills the cell and is cropped along one
// side. Smaller values (down to CollageMinZoom(), "contain") show more of
// it and leave the background visible, larger ones crop more. offset says
// where the photo sits in the room it has: 0 is the left / top edges
// together, 1 the right / bottom ones, 0.5 the middle. All sizes are in
// the same units (pixels of the collage), image is the photo after its
// quarter turns.

[[nodiscard]] double CollageMinZoom(QSizeF image, QSizeF cell);
[[nodiscard]] double CollageClampZoom(QSizeF image, QSizeF cell, double zoom);
// The whole photo in the coordinates of the cell (its top left is 0, 0).
[[nodiscard]] QRectF CollageImageRect(
	QSizeF image,
	QSizeF cell,
	double zoom,
	QPointF offset);

struct CollagePlacement {
	QRectF source; // The visible part of the photo, in its pixels.
	QRectF target; // Where it goes, in the coordinates of the cell.
};
[[nodiscard]] CollagePlacement PlaceCollagePhoto(
	QSizeF image,
	QSizeF cell,
	double zoom,
	QPointF offset);
// The offset after the photo was dragged by delta pixels of the cell.
[[nodiscard]] QPointF PanCollagePhoto(
	QSizeF image,
	QSizeF cell,
	double zoom,
	QPointF offset,
	QPointF delta);
struct CollageZoom {
	double zoom = 1.;
	QPointF offset = QPointF(0.5, 0.5);
};
// A new zoom that keeps the point of the photo under anchor (a point of
// the cell) in place, as far as the edges allow.
[[nodiscard]] CollageZoom ZoomCollagePhoto(
	QSizeF image,
	QSizeF cell,
	double zoom,
	QPointF offset,
	QPointF anchor,
	double newZoom);
// Pixels of a photo -> pixels of the photo mirrored (first) and turned.
[[nodiscard]] QTransform CollageOrientation(
	QSizeF image,
	int turns,
	bool mirror);

//
// The photos.
//

// The pixels of one photo with their smaller copies. Shared by every
// collage (and every undo step) that shows the photo. Thread safe.
class CollageSource final {
public:
	explicit CollageSource(std::shared_ptr<const ImageContent> content);

	[[nodiscard]] const std::shared_ptr<const ImageContent> &content() const {
		return _content;
	}
	[[nodiscard]] QSize size() const;

	// The smallest copy that still has scale of the full resolution
	// (0 < scale <= 1): the full image or one of the halved ones (made on
	// the first request). factor: its pixels per full size pixel.
	[[nodiscard]] QImage level(double scale, QSizeF *factor = nullptr) const;

	// The photo and the smaller copies made so far.
	[[nodiscard]] qint64 memoryUsage() const;
	// The same object by object, see LayerContent::memoryParts(): the
	// photo is reported as its ImageContent, so a photo that is a layer
	// somewhere too (the undo steps before "make a collage from the
	// layers" or after "convert to layers") is the same object there.
	void memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const;

private:
	const std::shared_ptr<const ImageContent> _content;
	mutable std::mutex _mutex;
	mutable std::vector<QImage> _levels; // 1/2, 1/4, ...

};

using CollageSourcePtr = std::shared_ptr<const CollageSource>;

[[nodiscard]] CollageSourcePtr MakeCollageSource(
	std::shared_ptr<const ImageContent> content);
// The half-size copy (2 x 2 averages) the pyramid is made of. The image
// must be Format_ARGB32_Premultiplied.
[[nodiscard]] QImage CollageHalfSized(const QImage &image);

struct CollageCell {
	CollageSourcePtr source; // Null: an empty cell.
	QString name; // The file or layer name, for "convert to layers".
	double zoom = 1.;
	QPointF offset = QPointF(0.5, 0.5);
	int turns = 0; // Quarter turns clockwise, 0..3.
	bool mirror = false; // Flipped horizontally (before the turns).

	friend bool operator==(
		const CollageCell &a,
		const CollageCell &b) = default;
};

enum class CollageBackground : uchar {
	None, // The layers under the collage are seen between the cells.
	Color,
	Gradient,
	Blur, // One of the photos, blurred.
};

struct CollageData {
	QSize size; // Pixels, the local rectangle of the layer.
	CollageGrid grid;
	// The photos in the order of the cells. Never shorter than
	// grid.count(), entries after that are kept but not shown.
	std::vector<CollageCell> cells;

	// Parts of the shorter side, see kCollageMax*.
	double spacing = 0.02;
	double margin = 0.02;
	double radius = 0.; // Never more than a half of a cell.

	CollageBackground background = CollageBackground::Color;
	QColor color1 = QColor(255, 255, 255); // The color, the gradient start.
	QColor color2 = QColor(30, 136, 229); // The gradient end.
	double gradientAngle = 90.; // Degrees, 0: left to right, 90: downwards.
	int blurCell = -1; // The photo of this cell, -1: the first one.
	double blurAmount = 0.5; // 0..1.
	double blurDim = 0.; // -1 (lighter) .. 1 (darker).

	friend bool operator==(
		const CollageData &a,
		const CollageData &b) = default;
};

// Everything clamped and repaired, cells padded to the grid. Normalized
// data comes back unchanged.
[[nodiscard]] CollageData NormalizedCollage(CollageData data);
// The template for the photos the collage has (in their order) that cuts
// the least off them at this size, spacing and margin: two landscape
// photos on a square go one above the other, two portraits side by
// side. Templates that come first win when there is little difference.
// What a new collage starts with.
[[nodiscard]] CollageGrid BestCollageGrid(const CollageData &data);
[[nodiscard]] int CollagePhotoCount(const CollageData &data); // With hidden.
// Pixels.
[[nodiscard]] double CollageSpacing(const CollageData &data);
[[nodiscard]] double CollageMargin(const CollageData &data);

// Pixels of the photo of a cell -> pixels of the collage, the cell being
// at rect. The cell must have a source.
[[nodiscard]] QTransform CollageCellTransform(
	const CollageCell &cell,
	QRectF rect);
// The size of the photo of a cell after its turns, empty without one.
[[nodiscard]] QSizeF CollageOrientedSize(const CollageCell &cell);

class CollageContent final : public LayerContent {
public:
	explicit CollageContent(CollageData data);

	QByteArray type() const override;
	QSize size() const override;
	QImage render(const ContentRequest &request) const override;
	// Everything this collage keeps alive, every photo once.
	qint64 memoryUsage() const override;
	// The photos are shared by all the undo steps of a collage (and with
	// image layers): each one is reported as an object of its own, the
	// undo history counts it once.
	void memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit
	) const override;

	[[nodiscard]] const CollageData &data() const {
		return _data;
	}
	// Only the cells that are shown (grid.count() of them).
	[[nodiscard]] const std::vector<QRectF> &cellRects() const {
		return _rects;
	}
	[[nodiscard]] int cellAt(QPointF point) const; // -1: none.
	[[nodiscard]] double cellRadius(int index) const; // Pixels.
	[[nodiscard]] std::vector<CollageSplitter> splitters() const;
	// The index of the cell whose photo the blurred background shows.
	[[nodiscard]] int blurCell() const;
	// Only the background, at the given scale. Null for
	// CollageBackground::None (and if cancelled).
	[[nodiscard]] QImage renderBackground(
		double scale,
		const std::atomic<bool> *cancel = nullptr) const;

private:
	const CollageData _data;
	const std::vector<QRectF> _rects;

};

[[nodiscard]] std::shared_ptr<const CollageContent> MakeCollageContent(
	CollageData data);
// Not null only for a collage.
[[nodiscard]] const CollageContent *AsCollage(const ContentPtr &content);

//
// Operations. Pure: they return changed copies.
//

struct CollagePhoto {
	std::shared_ptr<const ImageContent> content;
	QString name;
};
// The size a photo is kept at in a collage that has photos photos (with
// this one) and is canvas pixels large: what the collage can show when
// it is saved. The more photos, the smaller the cells: the longer side is
// at most kCollagePhotoMaxSide for up to 4 photos, then 2560, 2048 (up to
// 16) and 1600. And a photo never needs more pixels than cover the whole
// canvas (an empty canvas: not known, no such limit). Never larger than
// the photo itself.
[[nodiscard]] QSize CollagePhotoSize(
	QSize photo,
	int photos = 1,
	QSize canvas = QSize());
// Makes the loaded pictures ready for cells: scaled down to
// CollagePhotoSize(), converted. Null images are skipped, the pixels of
// every picture are released as soon as its copy is made. Worker thread
// (it touches every pixel).
[[nodiscard]] std::vector<CollagePhoto> PrepareCollagePhotos(
	std::vector<ImportedImage> images,
	int photos = 1,
	QSize canvas = QSize());

// Puts photos into the collage: the first one into the cell startCell if
// that is a shown cell (replacing its photo), the others into the empty
// cells; when there are more photos than room the collage gets more
// cells (BestCollageGrid() for all of its photos). At most
// kCollageMaxCells photos are kept: the ones over that are left out, and
// a collage that takes none of them comes back as it was. added gets the
// number of the photos that were put in.
[[nodiscard]] CollageData AddCollagePhotos(
	CollageData data,
	const std::vector<CollagePhoto> &photos,
	int startCell = -1,
	int *added = nullptr);
[[nodiscard]] CollageData SwapCollageCells(CollageData data, int a, int b);
// The cell stays, empty.
[[nodiscard]] CollageData RemoveCollagePhoto(CollageData data, int index);
// The cell goes away with its photo, its neighbours take the room.
[[nodiscard]] CollageData DeleteCollageCell(CollageData data, int index);
// Another grid for the same photos: empty cells between photos are
// closed up first, so a template for N photos shows all N.
[[nodiscard]] CollageData WithCollageGrid(CollageData data, CollageGrid grid);

// A document where the visible, unlocked image layers (bottom first, as
// many as fit) became the cells of one collage layer that takes the
// place of the lowest of them, followed by the extra photos. The collage
// is as large as the canvas. Without any photo it is an empty 2 x 2 grid.
// created gets the id of the collage layer.
[[nodiscard]] Document CollageFromLayers(
	const Document &document,
	const std::vector<CollagePhoto> &extra,
	const QString &name,
	LayerId *created = nullptr);

// The opposite: the collage layer is replaced by ordinary layers, one
// for the background (unless it is CollageBackground::None) and an image
// layer for every shown photo, placed and masked (the cell shape with
// its rounded corners) exactly as the collage showed it. The new layers
// take the opacity, the blend mode and the effects of the collage layer.
// Layers without a name get cellName with %1 replaced by their number.
// nullopt: not a collage, nothing to make layers from or cancelled.
// Worker thread (it renders the background).
[[nodiscard]] std::optional<Document> CollageToLayers(
	const Document &document,
	LayerId id,
	const QString &backgroundName,
	const QString &cellName,
	const std::atomic<bool> *cancel = nullptr);

// The size for a collage of the aspect ratio width : height that has
// about as many pixels as current.
[[nodiscard]] QSize CollageSizeForAspect(
	QSize current,
	int ratioWidth,
	int ratioHeight);
// Gives the collage another size. If the collage covered the whole canvas
// the canvas gets this size too: the other layers keep their place
// relative to its center and the whole-image crop is reset. Otherwise
// only the collage changes, around its center.
[[nodiscard]] Document ResizedCollage(
	const Document &document,
	LayerId id,
	QSize size);

// A new document that is one collage of these photos (a square canvas
// unless a size is given). Empty if there are no photos.
[[nodiscard]] Document CollageDocument(
	const std::vector<CollagePhoto> &photos,
	const QString &name,
	QSize canvas = QSize());

//
// Entry points for the rest of the app. Main thread.
//

// Loads the files off the main thread (EXIF orientation, sRGB, like
// LoadImage of oblivion_photo_core.h), builds a collage of them and shows
// the photo editor with the collage tool. Files that are not pictures are
// skipped, a toast is shown if none could be opened. options.document,
// options.tool and options.tab are set by this function, an empty
// options.fileName becomes "collage".
// Loading takes seconds: a toast says that the collage is being built
// until the editor opens, and only one collage is opened at a time (a call
// made meanwhile does nothing but show that toast again). A toast also
// says so when some of the files were left out (more than 16, or not
// pictures). Nothing is shown if the app was locked with the passcode
// meanwhile.
void ShowCollageEditor(
	std::shared_ptr<Ui::Show> show,
	QStringList paths,
	PhotoEditorOptions options = {});
// The same for pictures that are already in memory.
void ShowCollageEditor(
	std::shared_ptr<Ui::Show> show,
	std::vector<ImportedImage> images,
	PhotoEditorOptions options = {});
// Asks for the photos first (several files can be chosen).
void ChoosePhotosForCollage(
	std::shared_ptr<Ui::Show> show,
	PhotoEditorOptions options = {});

// The "collage" sub-test of OBLIVION_SELFTEST=photo_doc: template
// geometry, splitter limits, the fit maths, the pyramid, rendering and
// the conversions from and to layers. Pure, no lang strings.
[[nodiscard]] bool RunCollageSelfTest(QStringList &log);

} // namespace Oblivion::Photo
