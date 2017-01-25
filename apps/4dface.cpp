/*
 * 4dface: Real-time 3D face tracking and reconstruction from 2D video.
 *
 * File: apps/4dface.cpp
 *
 * Copyright 2015, 2016 Patrik Huber
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "helpers.hpp"

#include "eos/core/Landmark.hpp"
#include "eos/core/LandmarkMapper.hpp"
#include "eos/fitting/fitting.hpp"
#include "eos/fitting/orthographic_camera_estimation_linear.hpp"
#include "eos/fitting/contour_correspondence.hpp"
#include "eos/fitting/closest_edge_fitting.hpp"
#include "eos/fitting/RenderingParameters.hpp"
#include "eos/render/utils.hpp"
#include "eos/render/render.hpp"
#include "eos/render/texture_extraction.hpp"

#include "rcr/model.hpp"
#include "cereal/cereal.hpp"

#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

#include "Eigen/Dense"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/objdetect/objdetect.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "boost/program_options.hpp"
#include "boost/filesystem.hpp"

#include <vector>
#include <iostream>

using namespace eos;
namespace po = boost::program_options;
namespace fs = boost::filesystem;
using cv::Mat;
using cv::Vec2f;
using cv::Vec3f;
using cv::Vec4f;
using cv::Rect;
using std::cout;
using std::endl;
using std::vector;
using std::string;

void draw_axes_topright(float r_x, float r_y, float r_z, cv::Mat image);

/**
 * This app demonstrates facial landmark tracking, estimation of the 3D pose
 * and fitting of the shape model of a 3D Morphable Model from a video stream,
 * and merging of the face texture.
 */
int main(int argc, char *argv[])
{
	fs::path modelfile, inputvideo, facedetector, landmarkdetector, mappingsfile, contourfile, edgetopologyfile, blendshapesfile;
	try {
		po::options_description desc("Allowed options");
		desc.add_options()
			("help,h",
				"display the help message")
			("morphablemodel,m", po::value<fs::path>(&modelfile)->required()->default_value("../share/sfm_shape_3448.bin"),
				"a Morphable Model stored as cereal BinaryArchive")
			("facedetector,f", po::value<fs::path>(&facedetector)->required()->default_value("../share/haarcascade_frontalface_alt2.xml"),
				"full path to OpenCV's face detector (haarcascade_frontalface_alt2.xml)")
			("landmarkdetector,l", po::value<fs::path>(&landmarkdetector)->required()->default_value("../share/face_landmarks_model_rcr_68.bin"),
				"learned landmark detection model")
			("mapping,p", po::value<fs::path>(&mappingsfile)->required()->default_value("../share/ibug2did.txt"),
				"landmark identifier to model vertex number mapping")
			("model-contour,c", po::value<fs::path>(&contourfile)->required()->default_value("../share/model_contours.json"),
				"file with model contour indices")
			("edge-topology,e", po::value<fs::path>(&edgetopologyfile)->required()->default_value("../share/sfm_3448_edge_topology.json"),
				"file with model's precomputed edge topology")
			("blendshapes,b", po::value<fs::path>(&blendshapesfile)->required()->default_value("../share/expression_blendshapes_3448.bin"),
				"file with blendshapes")
			("input,i", po::value<fs::path>(&inputvideo),
				"input video file. If not specified, camera 0 will be used.")
			;
		po::variables_map vm;
		po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
		if (vm.count("help")) {
			cout << "Usage: 4dface [options]" << endl;
			cout << desc;
			return EXIT_FAILURE;
		}
		po::notify(vm);
	}
	catch (const po::error& e) {
		cout << "Error while parsing command-line arguments: " << e.what() << endl;
		cout << "Use --help to display a list of options." << endl;
		return EXIT_FAILURE;
	}

	// Load the Morphable Model and the LandmarkMapper:
	morphablemodel::MorphableModel morphable_model = morphablemodel::load_model(modelfile.string());
	core::LandmarkMapper landmark_mapper = mappingsfile.empty() ? core::LandmarkMapper() : core::LandmarkMapper(mappingsfile);

	fitting::ModelContour model_contour = contourfile.empty() ? fitting::ModelContour() : fitting::ModelContour::load(contourfile.string());
	fitting::ContourLandmarks ibug_contour = fitting::ContourLandmarks::load(mappingsfile.string());

	rcr::detection_model rcr_model;
	// Load the landmark detection model:
	try {
		rcr_model = rcr::load_detection_model(landmarkdetector.string());
	}
	catch (const cereal::Exception& e) {
		cout << "Error reading the RCR model " << landmarkdetector << ": " << e.what() << endl;
		return EXIT_FAILURE;
	}

	// Load the face detector from OpenCV:
	cv::CascadeClassifier face_cascade;
	if (!face_cascade.load(facedetector.string()))
	{
		cout << "Error loading the face detector " << facedetector << "." << endl;
		return EXIT_FAILURE;
	}

	// cv::VideoCapture cap;
	// if (inputvideo.empty()) {
	// 	cap.open(0); // no file given, open the default camera
	// }
	// else {
	// 	cap.open(inputvideo.string());
	// }
	// if (!cap.isOpened()) {
	// 	cout << "Couldn't open the given file or camera 0." << endl;
	// 	return EXIT_FAILURE;
	// }

	vector<morphablemodel::Blendshape> blendshapes = morphablemodel::load_blendshapes(blendshapesfile.string());

	morphablemodel::EdgeTopology edge_topology = morphablemodel::load_edge_topology(edgetopologyfile.string());

	cv::namedWindow("video", 1);
	cv::namedWindow("render", 1);

	Mat frame, unmodified_frame;

	bool have_face = false;
	rcr::LandmarkCollection<Vec2f> current_landmarks;
	Rect current_facebox;
	WeightedIsomapAveraging isomap_averaging(90.f); // merge all triangles that are facing <60� towards the camera
	PcaCoefficientMerging pca_shape_merging;

	// for (;;)
	// {
	// cap >> frame; // get a new frame from camera
	std::string img_path = inputvideo.string();
	frame = cv::imread(img_path,1);

	int h = frame.cols;
	int w = frame.rows;


	if (frame.empty()) { // stop if we're at the end of the video
		exit(0);
	}

	// We do a quick check if the current face's width is <= 50 pixel. If it is, we re-initialise the tracking with the face detector.
	if (have_face && get_enclosing_bbox(rcr::to_row(current_landmarks)).width <= 50) {
		cout << "Reinitialising because the face bounding-box width is <= 50 px" << endl;
		have_face = false;
	}

	unmodified_frame = frame.clone();

	cv::Mat polygon_mask = cv::Mat(w,h,CV_8U,cv::Scalar(0));

	std::vector<cv::Point> landmark_polygon;


	if (!have_face) {
		// Run the face detector and obtain the initial estimate using the mean landmarks:
		vector<Rect> detected_faces;
		face_cascade.detectMultiScale(unmodified_frame, detected_faces, 1.2, 2, 0, cv::Size(110, 110));
		if (detected_faces.empty()) {
			cv::imshow("video", frame);
			cv::waitKey(30);
			exit(0);
		}
		cv::rectangle(frame, detected_faces[0], { 255, 0, 0 });
		// Rescale the V&J facebox to make it more like an ibug-facebox:
		// (also make sure the bounding box is square, V&J's is square)
		Rect ibug_facebox = rescale_facebox(detected_faces[0], 0.85, 0.2);

		current_landmarks = rcr_model.detect(unmodified_frame, ibug_facebox);
		rcr::draw_landmarks(frame, current_landmarks);


		have_face = true;

		// cropping here
		std::vector<int> indices = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,26,25,24,23,22,21,20,19,18,17};
		for (int ic=0; ic<indices.size(); ic++){
			cv::Point tmp((int)current_landmarks[indices[ic]].coordinates[0],(int)current_landmarks[indices[ic]].coordinates[1]);
			std::cout<<tmp<<std::endl;
			landmark_polygon.push_back(tmp);
		}
		std::cout<<landmark_polygon.size()<<std::endl;
		cv::Mat tmp_copy = unmodified_frame.clone();
		cv::GaussianBlur(unmodified_frame,tmp_copy, cv::Size(3,3), 0, 0);
		cv::fillConvexPoly(polygon_mask, landmark_polygon, cv::Scalar(255), 28);
		cv::imwrite("lalala.jpg",polygon_mask);
		std::cout<<"FILL WORKS"<<std::endl;
		tmp_copy.copyTo(unmodified_frame, polygon_mask);
	}
	else {
		// We already have a face - track and initialise using the enclosing bounding
		// box from the landmarks from the last frame:
		auto enclosing_bbox = get_enclosing_bbox(rcr::to_row(current_landmarks));
		enclosing_bbox = make_bbox_square(enclosing_bbox);
		current_landmarks = rcr_model.detect(unmodified_frame, enclosing_bbox);
		rcr::draw_landmarks(frame, current_landmarks, { 0, 255, 0 }); // green, the new optimised landmarks
	}

	// Fit the 3DMM. First, estimate the pose:
	vector<Vec2f> image_points; // the 2D landmark points for which a mapping to the 3D model exists. A subset of all the detected landmarks.
	vector<Vec4f> model_points; // the corresponding points in the 3D shape model
	vector<int> vertex_indices; // their vertex indices
	std::tie(image_points, model_points, vertex_indices) = get_corresponding_pointset(current_landmarks, landmark_mapper, morphable_model);
	auto scaled_ortho_projection = fitting::estimate_orthographic_projection_linear(image_points, model_points, true, frame.rows);
	auto rendering_params = fitting::RenderingParameters(scaled_ortho_projection, frame.cols, frame.rows);

	// Given the estimated pose, find 2D-3D contour correspondences of the front-facing face contour:
	// These are the additional contour correspondences we're going to find and then use:
	vector<Vec2f> image_points_contour;
	vector<Vec4f> model_points_contour;
	vector<int> vertex_indices_contour;
	// For each 2D contour landmark, get the corresponding 3D vertex point and vertex id:
	auto yaw_angle = glm::degrees(glm::eulerAngles(rendering_params.get_rotation())[1]);
	std::tie(image_points_contour, model_points_contour, vertex_indices_contour) = fitting::get_contour_correspondences(rcr_to_eos_landmark_collection(current_landmarks), ibug_contour, model_contour, yaw_angle, morphable_model, rendering_params.get_modelview(), rendering_params.get_projection(), fitting::get_opencv_viewport(frame.cols, frame.rows));
	// Add the contour correspondences to the set of landmarks that we use for the fitting:
	model_points = concat(model_points, model_points_contour);
	vertex_indices = concat(vertex_indices, vertex_indices_contour);
	image_points = concat(image_points, image_points_contour);

	// Occluded edge fitting: Fit the occluding model contour to the detected contour landmarks:
	vector<Eigen::Vector2f> occluding_contour_landmarks;
	if (yaw_angle >= 0.0f) // positive yaw = subject looking to the left
	{ // the left contour is the occluding one we want to use ("away-facing")
		auto contour_landmarks_ = rcr::filter(current_landmarks, ibug_contour.left_contour);
		std::for_each(begin(contour_landmarks_), end(contour_landmarks_), [&occluding_contour_landmarks](auto&& lm) { occluding_contour_landmarks.push_back({ lm.coordinates[0], lm.coordinates[1] }); });	
	}
	else {
		auto contour_landmarks_ = rcr::filter(current_landmarks, ibug_contour.right_contour);
		std::for_each(begin(contour_landmarks_), end(contour_landmarks_), [&occluding_contour_landmarks](auto&& lm) { occluding_contour_landmarks.push_back({ lm.coordinates[0], lm.coordinates[1] }); });
	}

	auto mean_mesh = morphable_model.get_mean();
	auto edge_correspondences = fitting::find_occluding_edge_correspondences(mean_mesh, edge_topology, rendering_params, occluding_contour_landmarks, 200.0f);
	// Add new edge correspondences:
	image_points = fitting::concat(image_points, edge_correspondences.first);
	vertex_indices = fitting::concat(vertex_indices, edge_correspondences.second);
	for (const auto& e : edge_correspondences.second)
	{
		model_points.emplace_back(morphable_model.get_shape_model().get_mean_at_point(e));
	}

	// Re-estimate the pose with all landmarks, including the contour landmarks:
	scaled_ortho_projection = fitting::estimate_orthographic_projection_linear(image_points, model_points, true, frame.rows);
	rendering_params = fitting::RenderingParameters(scaled_ortho_projection, frame.cols, frame.rows);
	Mat affine_cam = fitting::get_3x4_affine_camera_matrix(rendering_params, frame.cols, frame.rows);

	// Fit the PCA shape model and expression blendshapes:
	vector<float> shape_coefficients, blendshape_coefficients;
	Mat shape_instance = fitting::fit_shape(affine_cam, morphable_model, blendshapes, image_points, vertex_indices, 10.0f, boost::none, shape_coefficients, blendshape_coefficients);

	// Draw the 3D pose of the face:
	draw_axes_topright(glm::eulerAngles(rendering_params.get_rotation())[0], glm::eulerAngles(rendering_params.get_rotation())[1], glm::eulerAngles(rendering_params.get_rotation())[2], frame);

	std::cout<<"HELP"<<std::endl;
	// Get the fitted mesh, extract the texture:
	render::Mesh mesh = morphablemodel::detail::sample_to_mesh(shape_instance, morphable_model.get_color_model().get_mean(), morphable_model.get_shape_model().get_triangle_list(), morphable_model.get_color_model().get_triangle_list(), morphable_model.get_texture_coordinates());
	Mat isomap = render::extract_texture(mesh, affine_cam, unmodified_frame, true, render::TextureInterpolation::NearestNeighbour, 2048);

	std::cout<<"HELP AGAIN"<<std::endl;
	// Merge the isomaps - add the current one to the already merged ones:
	//Mat merged_isomap = isomap_averaging.add_and_merge(isomap);
	// Same for the shape:
	//shape_coefficients = pca_shape_merging.add_and_merge(shape_coefficients);
	auto merged_shape = morphable_model.get_shape_model().draw_sample(shape_coefficients) + morphablemodel::to_matrix(blendshapes) * Mat(blendshape_coefficients);
	std::cout<<"HELP HERE ALSO"<<std::endl;
	render::Mesh merged_mesh = morphablemodel::detail::sample_to_mesh(merged_shape, morphable_model.get_color_model().get_mean(), morphable_model.get_shape_model().get_triangle_list(), morphable_model.get_color_model().get_triangle_list(), morphable_model.get_texture_coordinates());

	// Render the model in a separate window using the estimated pose, shape and merged texture:
	std::cout<<"HELLO"<<std::endl;
	Mat rendering;
	auto modelview_no_translation = rendering_params.get_modelview();
	modelview_no_translation[3][0] = 0;
	modelview_no_translation[3][1] = 0;
	std::tie(rendering, std::ignore) = render::render(merged_mesh, modelview_no_translation, glm::ortho(-130.0f, 130.0f, -130.0f, 130.0f), 256, 256, render::create_mipmapped_texture(isomap), true, false, false);
	cv::imshow("render", rendering);

	cv::imshow("video", frame);
	auto key = cv::waitKey(30);
	if (key == 'r') {
		have_face = false;
		isomap_averaging = WeightedIsomapAveraging(0.f);
	}
	// if (key == 's') {
		// save an obj + current merged isomap to the disk:
		render::Mesh neutral_expression = morphablemodel::detail::sample_to_mesh(morphable_model.get_shape_model().draw_sample(shape_coefficients), morphable_model.get_color_model().get_mean(), morphable_model.get_shape_model().get_triangle_list(), morphable_model.get_color_model().get_triangle_list(), morphable_model.get_texture_coordinates());
		render::write_textured_obj(neutral_expression, "current_merged.obj");
		cv::imwrite(img_path+".merged_isomap.jpg", isomap);
		cv::imwrite(img_path+".rendering.jpg", rendering);
	// }
	//}
	return EXIT_SUCCESS;
};

/**
 * @brief Draws 3D axes onto the top-right corner of the image. The
 * axes are oriented corresponding to the given angles.
 *
 * @param[in] r_x Pitch angle, in radians.
 * @param[in] r_y Yaw angle, in radians.
 * @param[in] r_z Roll angle, in radians.
 * @param[in] image The image to draw onto.
 */
void draw_axes_topright(float r_x, float r_y, float r_z, cv::Mat image)
{
	const glm::vec3 origin(0.0f, 0.0f, 0.0f);
	const glm::vec3 x_axis(1.0f, 0.0f, 0.0f);
	const glm::vec3 y_axis(0.0f, 1.0f, 0.0f);
	const glm::vec3 z_axis(0.0f, 0.0f, 1.0f);

	const auto rot_mtx_x = glm::rotate(glm::mat4(1.0f), r_x, glm::vec3{ 1.0f, 0.0f, 0.0f });
	const auto rot_mtx_y = glm::rotate(glm::mat4(1.0f), r_y, glm::vec3{ 0.0f, 1.0f, 0.0f });
	const auto rot_mtx_z = glm::rotate(glm::mat4(1.0f), r_z, glm::vec3{ 0.0f, 0.0f, 1.0f });
	const auto modelview = rot_mtx_z * rot_mtx_x * rot_mtx_y;

	const auto viewport = fitting::get_opencv_viewport(image.cols, image.rows);
	const float aspect = static_cast<float>(image.cols) / image.rows;
	const auto ortho_projection = glm::ortho(-3.0f * aspect, 3.0f * aspect, -3.0f, 3.0f);
	const auto translate_topright = glm::translate(glm::mat4(1.0f), glm::vec3(0.7f, 0.65f, 0.0f));
	const auto o_2d = glm::project(origin, modelview, translate_topright * ortho_projection, viewport);
	const auto x_2d = glm::project(x_axis, modelview, translate_topright * ortho_projection, viewport);
	const auto y_2d = glm::project(y_axis, modelview, translate_topright * ortho_projection, viewport);
	const auto z_2d = glm::project(z_axis, modelview, translate_topright * ortho_projection, viewport);
	cv::line(image, cv::Point2f{ o_2d.x, o_2d.y }, cv::Point2f{ x_2d.x, x_2d.y }, { 0, 0, 255 });
	cv::line(image, cv::Point2f{ o_2d.x, o_2d.y }, cv::Point2f{ y_2d.x, y_2d.y }, { 0, 255, 0 });
	cv::line(image, cv::Point2f{ o_2d.x, o_2d.y }, cv::Point2f{ z_2d.x, z_2d.y }, { 255, 0, 0 });
};
