package flump.display {

import loom2d.math.Point;

import flump.mold.AtlasMold;
import flump.mold.AtlasTextureMold;
import flump.mold.MovieMold;

import loom2d.textures.Texture;

/**
 * A Factory for creating SymbolCreators that is given some context as a Library is assembled.
 */
public interface CreatorFactory {
    function createImageCreator (mold :AtlasTextureMold, texture :Texture, origin :Point,
        symbol :String) :ImageCreator;

    function createMovieCreator (mold :MovieMold, frameRate :Number) :MovieCreator;

    function consumingAtlasMold (mold :AtlasMold) :void;
}
}
