#include "viz.h"

const int AA_Samples = 4;
const int WIN_Height = 1024;
const int WIN_Width = 768;

const uint16_t fps = 60;
const uint16_t duration = 100; //(ms)
const uint16_t FS = 44100;
const size_t n_samples = FS * duration / 1000;
const size_t buf_size = FS / fps;
const size_t fft_size = 2<<13; //2^14

float top_color[] = {0.827/2.0, 0.149/2.0, 0.008/2.0, 1.0};
float bot_color[] = {0.275/2.0, 0.282/2.0, 0.294/2.0, 1.0};
float line_color[] = {0.275, 0.282, 0.294, 1.0};

void init_x_data(std::vector<float>&, const size_t);
void update_y_buffer(FFT&);
void update_x_buffer(std::vector<float>&);
void init_buffers(Program&, std::vector<float>&, FFT&);

size_t fft_output_size = fft_size/2+1;
const float d_freq = (float) FS / (float) fft_output_size;
const float fft_scale = 1.0f/((float)(n_samples/2+1)*32768.0f);
const float slope = 0.5f;
const float offset = 1.0f;
const float gravity = 0.008f;

glm::mat4 view = glm::lookAt(
    glm::vec3(0.8f, 0.0f, 2.0f),
    glm::vec3(0.0f, 0.0f, 0.0f),
    glm::vec3(0.0f, 1.0f, 0.0f)
); 
glm::mat4 projection = glm::perspective(glm::radians(45.0f),  (float)WIN_Height / (float)WIN_Width, 1.0f, 10.0f);
glm::mat4 transformation = glm::ortho(-1.0,1.0,-1.0,1.0,-1.0,1.0);

void set_transformation(Program&);

// buffers and the corresponding vectors
GLuint vao_spec;

const size_t output_size = 100; 
GLuint y_buffer;
GLuint x_buffer;
// transform feedback buffers for gravity
GLuint fb1, fb2;

// buffers and shaders for dB lines
GLuint vao_db;

const float lines[]  = {
	-1.0,  0.0, 1.0,  0.0, //   0dB
	-1.0, -0.5, 1.0, -0.5, // -10dB
	-1.0, -1.0, 1.0, -1.0, // -20dB
	-1.0, -1.5, 1.0, -1.5, // -30dB
	-1.0, -2.0, 1.0, -2.0, // -40dB
	-1.0, -2.5, 1.0, -2.5, // -50dB
	-1.0, -3.0, 1.0, -3.0, // -60dB
	-1.0, -3.5, 1.0, -3.5, // -70dB
	-1.0, -4.0, 1.0, -4.0  // -80dB
};
GLuint line_buffer;
void init_lines(Program&);

// handle window resizing
void framebuffer_size_callback(GLFWwindow* window, int width, int height){
	glViewport(0, 0, width, height);
} 

int main(){
	
	// init GLFW
	if(!glfwInit()){
		fprintf(stderr, "GLFW init failed!\n");
		return -1;
	}
	
	glfwWindowHint(GLFW_SAMPLES, AA_Samples);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	
	std::stringstream title;
	title << "Spectrum (fmax=" << output_size * d_freq << "Hz)" ;
	
	GLFWwindow* window;
	window = glfwCreateWindow( WIN_Height, WIN_Width, title.str().c_str(), NULL, NULL);
	if( window == NULL ){
		fprintf(stderr, "GLFW Window init failed!\n");
		glfwTerminate();
		return -1;
	}

	glfwMakeContextCurrent(window); // init GLEW
	glewExperimental = true;

	if(glewInit() != GLEW_OK){
		fprintf(stderr, "GLEW init failed!\n");
		return -1;	
	}
	
	Program sh_spec;
	Program sh_db;
	init_shaders(sh_spec, sh_db);
	
	FFT fft(fft_size);

	std::vector<float> x_data(output_size);

	init_x_data(x_data, output_size);
	init_buffers(sh_spec, x_data,fft);
	init_lines(sh_db);
	
	set_transformation(sh_spec);
	set_transformation(sh_db);
	
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

	Input fifo("/tmp/mpd.fifo");
//	Pulse p(Pulse::get_default_sink(), buf_size);
	std::vector<int16_t> v_data(n_samples);
	
	GLint arg_gravity_old = sh_spec.get_attrib("gravity_old");
	
	if (fifo.is_open()){
		// init read thread
		std::thread th_read;
		std::thread th_fps;
			
		// handle resizing	
		glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);	
		do{
			th_fps = std::thread([]{usleep(1000000 / fps);});	
			update_y_buffer(fft);
			
			//clear and draw		
			glClear(GL_COLOR_BUFFER_BIT);
			
			// render Lines
			sh_db.use();
			glBindVertexArray(vao_db);
			glDrawArrays(GL_LINES, 0, sizeof(lines) / sizeof(float));

			// render bars
			sh_spec.use();	
			glBindVertexArray(vao_spec);
			// use second feedback buffer for drawing
			glBindBuffer(GL_ARRAY_BUFFER, fb2);
			glVertexAttribPointer(arg_gravity_old, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float), NULL);	
			
			// bind fist feedback buffer as TF buffer
			glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, fb1);
			glBeginTransformFeedback(GL_TRIANGLES);

			glDrawArrays(GL_POINTS, 0, output_size);
			// disable TF
			glEndTransformFeedback();	

			//undbind both feedback buffers and swap them
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
			std::swap(fb1,fb2);

			// Swap buffers
			glfwSwapBuffers(window);
			glfwPollEvents();
			
			// start read thread	
			th_read = std::thread([&]{ fifo.read_fifo(v_data, fft); });
			//p.read(v_data);
			//fft.calculate(v_data);		
			th_fps.join();
			th_read.join();
		} // Wait until window is closed
		while(glfwWindowShouldClose(window) == 0);
	}	
	// clear buffers
	glDeleteBuffers(1, &y_buffer);
	glDeleteBuffers(1, &x_buffer);
	glDeleteBuffers(1, &line_buffer);
	glDeleteBuffers(1, &fb1);
	glDeleteBuffers(1, &fb2);
	glDeleteVertexArrays(1, &vao_db);
	glDeleteVertexArrays(1, &vao_spec);
	

	glfwTerminate();

	return 0;
}

// update y_data buffer
void update_y_buffer(FFT& fft){
	glBindBuffer(GL_ARRAY_BUFFER, y_buffer);
	glBufferData(GL_ARRAY_BUFFER, output_size * sizeof(fftw_complex), fft.output, GL_DYNAMIC_DRAW);
}

void update_x_buffer(std::vector<float>& data){
	glBindBuffer(GL_ARRAY_BUFFER, x_buffer);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), &data[0], GL_DYNAMIC_DRAW);
}

void init_buffers(Program& sh_spec, std::vector<float>& x_data,FFT& fft){
	// generate spectrum vao
	glGenVertexArrays(1, &vao_spec);

	glBindVertexArray(vao_spec);
	// Generate the y_position buffer
	glGenBuffers(1, &y_buffer);
	// bind the y_buffer for modification
	glBindBuffer(GL_ARRAY_BUFFER, y_buffer);
	// fill the y_buffer
	glBufferData(GL_ARRAY_BUFFER, output_size * sizeof(fftw_complex), fft.output, GL_DYNAMIC_DRAW);
	GLint arg_fft_output = sh_spec.get_attrib("a");
	glVertexAttribPointer(arg_fft_output, 2, GL_DOUBLE, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(arg_fft_output);
	
	
	/* X position buffer */
	glGenBuffers(1, &x_buffer);

	glBindBuffer(GL_ARRAY_BUFFER, x_buffer);
	glBufferData(GL_ARRAY_BUFFER, x_data.size() * sizeof(float), &x_data[0], GL_DYNAMIC_DRAW);

	GLint arg_x_data = sh_spec.get_attrib("x");
	glVertexAttribPointer(arg_x_data, 1, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(arg_x_data);
	
	// init feedback buffers
	glGenBuffers(1, &fb1);
	glGenBuffers(1, &fb2);
	glBindBuffer(GL_ARRAY_BUFFER, fb1);
	glBufferData(GL_ARRAY_BUFFER, output_size * 12 *sizeof(float), 0, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, fb2);
	glBufferData(GL_ARRAY_BUFFER, output_size * 12 *sizeof(float), 0, GL_DYNAMIC_DRAW);
	// use second feedback buffer for drawing
	GLint arg_gravity_old = sh_spec.get_attrib("gravity_old");
	glVertexAttribPointer(arg_gravity_old, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float), NULL);
	glEnableVertexAttribArray(arg_gravity_old);	

	sh_spec.use();
	// init the shader uniform values
	GLint i_fft_scale = sh_spec.get_uniform("fft_scale");
	glUniform1f(i_fft_scale, fft_scale);	

	GLint i_slope = sh_spec.get_uniform("slope");
	glUniform1f(i_slope, slope);
	
	GLint i_offset = sh_spec.get_uniform("offset");
	glUniform1f(i_offset, offset);

	GLint i_width = sh_spec.get_uniform("width");
	glUniform1f(i_width, 1.5f/(float)output_size);
	
	// set bar color gradients	
	GLint i_top_color = sh_spec.get_uniform("top_color");
	glUniform4fv(i_top_color, 1, top_color);

	GLint i_bot_color = sh_spec.get_uniform("bot_color");
	glUniform4fv(i_bot_color, 1, bot_color);

	// set gravity
	GLint i_gravity = sh_spec.get_uniform("gravity");
	glUniform1f(i_gravity, gravity);	
}

void init_lines(Program& sh_lines){
	glGenVertexArrays(1, &vao_db);
	
	glBindVertexArray(vao_db);
	
	glGenBuffers(1, &line_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, line_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_DYNAMIC_DRAW);
	
	GLint arg_line_vert = sh_lines.get_attrib("pos");
	glVertexAttribPointer(arg_line_vert, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(arg_line_vert);

	sh_lines.use();
	
	GLint i_slope = sh_lines.get_uniform("slope");
	glUniform1f(i_slope, slope);
	
	GLint i_offset = sh_lines.get_uniform("offset");
	glUniform1f(i_offset, offset);

	GLint i_line_color = sh_lines.get_uniform("line_color");
	glUniform4fv(i_line_color, 1, line_color);
}

void set_transformation(Program& sh){
	sh.use();
	
	GLint i_trans = sh.get_uniform("trans");
	glUniformMatrix4fv(i_trans, 1, GL_FALSE, glm::value_ptr(transformation));
}

void init_x_data(std::vector<float>& vec, const size_t size){
	vec.resize(size);
	for(unsigned int i = 0; i < size; i++){
		vec[i] = (((float) i + 0.5) - ((float)size * 0.5)) / ((float)size * 0.5);
	}
}
