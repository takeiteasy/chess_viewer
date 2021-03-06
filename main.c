#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "3rdparty/glad.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "3rdparty/linalgb.h"
#define SLIM_HASH_IMPLEMENTATION
#include "3rdparty/slim_hash.h"
#include "obj.h"
#include "helpers.h"
#include "thread_t/threads.h"
#include <pcre.h>

static const int SCREEN_WIDTH  = 640,
                 SCREEN_HEIGHT = 480;

static SDL_Window* window;
static SDL_GLContext context;
static pcre* re;

#define DEFAULT_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
#define BOARD_STEP 6.f
#define BOARD_TOP -21.f
#define DRAG_SPEED 15.f
#define BUFFER_SIZE 1024
#define PORT 8888

static int client_connected = 0;
static char* current_fen = DEFAULT_FEN;

#define LOAD_PIECE(x, c) \
obj_t x; \
load_obj(&x, "res/" #x ".obj"); \
if (c) { \
  dict_put(&piece_map, (char)c, &x); \
  dict_put(&piece_map, (char)(c - 32), &x); \
}

#undef GLAD_DEBUG

#ifdef GLAD_DEBUG
void pre_gl_call(const char *name, void *funcptr, int len_args, ...) {
  printf("Calling: %s (%d arguments)\n", name, len_args);
}
#endif

char* glGetError_str(GLenum err) {
  switch (err) {
    case GL_INVALID_ENUM:                  return "INVALID_ENUM"; break;
    case GL_INVALID_VALUE:                 return "INVALID_VALUE"; break;
    case GL_INVALID_OPERATION:             return "INVALID_OPERATION"; break;
    case GL_STACK_OVERFLOW:                return "STACK_OVERFLOW"; break;
    case GL_STACK_UNDERFLOW:               return "STACK_UNDERFLOW"; break;
    case GL_OUT_OF_MEMORY:                 return "OUT_OF_MEMORY"; break;
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION"; break;
    default:
      return "Unknown Error";
  }
}

void post_gl_call(const char *name, void *funcptr, int len_args, ...) {
  GLenum err = glad_glGetError();
  if (err != GL_NO_ERROR) {
    fprintf(stderr, "ERROR %d (%s) in %s\n", err, glGetError_str(err), name);
    abort();
  }
}

void cleanup() {
  SDL_DestroyWindow(window);
  SDL_GL_DeleteContext(context);
  free(re);
  printf("Goodbye!\n");
}

static char grid[64];

SH_GEN_DECL(dict, char, obj_t*);
SH_GEN_HASH_IMPL(dict, char, obj_t*);
static struct dict piece_map;

static char valid_fen_chars[] = {
  'p', 'r', 'n', 'b', 'k', 'q',
  'P', 'R', 'N', 'B', 'K', 'Q'
};

void fen_to_grid(const char* fen) {
  int fen_total = 0;
  for (int i = 0; i < strlen(fen); ++i) {
    char c = fen[i];
    if (c == ' ' || c == '\n')
      break;
    if (c == '/')
      continue;
    
    if (c >= '1' && c <= '8')
      fen_total += ((int)c - 48);
    else {
      int is_valid = 0;
      for (int j = 0; j < 12; ++j) {
        if (c == valid_fen_chars[j]) {
          is_valid = 1;
          break;
        }
      }
      if (is_valid)
        fen_total += 1;
      else {
        fprintf(stderr, "ERROR! Invalid FEN char (%c) in \"%s\"\n", c, fen);
        fen_to_grid(DEFAULT_FEN);
        return;
      }
    }
  }
  if (fen_total < 64) {
    fprintf(stderr, "ERROR! Invalid FEN string \"%s\"\n", fen);
    fen_to_grid(DEFAULT_FEN);
  }
  
  int cur_row = 0, cur_col = 0;
  for (int i = 0; i < strlen(fen); ++i) {
    char c = fen[i];
    if (c == ' ')
      break;
    
    if (c == '/') {
      cur_row += 1;
      cur_col  = 0;
      continue;
    }
    
    if (c >= '1' && c <= '8') {
      int v = (int)c - 48;
      for (int j = 0; j < v; ++j) {
        grid[cur_row * 8 + cur_col++] = 'X';
      }
    } else {
      grid[cur_row * 8 + cur_col++] = c;
    }
  }
  
#ifdef GLAD_DEBUG
  printf("---------------\n");
  for (int j = 0; j < 8; ++j) {
    for (int k = 0; k < 8; ++k) {
      printf("%c ", grid[j * 8 + k]);
    }
    printf("\n");
  }
  printf("---------------\n");
#endif
}

void server_thread(void* arg) {
  int server_fd, client_fd, err;
  struct sockaddr_in server, client;
  char buf[BUFFER_SIZE];
  
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    fprintf(stderr, "Could not create socket\n");
    exit(-1);
  }
  
  server.sin_family = AF_INET;
  server.sin_port = htons(PORT);
  server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  
  int opt_val = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof opt_val);
  
  err = bind(server_fd, (struct sockaddr *)&server, sizeof(server));
  if (err < 0) {
    fprintf(stderr, "Could not bind socket\n");
    exit(-1);
  }
  
  err = listen(server_fd, 128);
  if (err < 0) {
    fprintf(stderr, "Could not listen on socket\n");
    exit(-1);
  }
  
  printf("Server is listening on %d\n", PORT);
  
  const char* error;
  int error_offset;
  int offset_count;
  int offsets[8];
  re = pcre_compile("^([prbnqk1-8]+\/?){8}.*$", PCRE_CASELESS, &error, &error_offset, NULL);
  if (re == NULL) {
    fprintf(stderr, "Failed to create FEN regex!\n");
    exit(-1);
  }
  
  socklen_t client_len = sizeof(client);
  
  while (1) {
    client_fd = accept(server_fd, (struct sockaddr *) &client, &client_len);
    
    if (client_fd < 0) {
      fprintf(stderr, "Could not establish new connection\n");
      exit(-1);
    }
    
    client_connected = 1;
    
    while (1) {
      int read = recv(client_fd, buf, BUFFER_SIZE, 0);
      
      if (!read)
        break;
      
      if (read < 0) {
        fprintf(stderr, "Client read failed\n");
        exit(-1);
      }
      
      if (pcre_exec(re, NULL, buf, read, 0, 0, offsets, 8) > 0) {
        printf("%s\n", buf);
        fen_to_grid(buf);
      }
      
      thrd_yield();
    }
    
    client_connected = 0;
  }
}

int main(int argc, const char* argv[]) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    fprintf(stderr, "Failed to initalize SDL!\n");
    return -1;
  }
  
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  
  window = SDL_CreateWindow(argv[0],
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            SCREEN_WIDTH, SCREEN_HEIGHT,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN );
  if (!window) {
    fprintf(stderr, "Failed to create SDL window!\n");
    return -1;
  }
  
  context = SDL_GL_CreateContext(window);
  if (!context) {
    fprintf(stderr, "Failed to create OpenGL context!\n");
    return -1;
  }
  
  if (!gladLoadGL()) {
    fprintf(stderr, "Failed to load GLAD!\n");
    return -1;
  }
  
#ifdef GLAD_DEBUG
  glad_set_pre_callback(pre_gl_call);
#endif
  
  glad_set_post_callback(post_gl_call);
  
  printf("Vendor:   %s\n", glGetString(GL_VENDOR));
  printf("Renderer: %s\n", glGetString(GL_RENDERER));
  printf("Version:  %s\n", glGetString(GL_VERSION));
  printf("GLSL:     %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
  
  glClearColor(0.93, 0.93, 0.93, 1.0f);
  glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glCullFace(GL_BACK);
  
  float plane_vertices[] = {
     1.f,  1.f, 0.0f,  1.0f, 1.0f,
     1.f, -1.f, 0.0f,  1.0f, 0.0f,
    -1.f, -1.f, 0.0f,  0.0f, 0.0f,
    -1.f,  1.f, 0.0f,  0.0f, 1.0f
  };
  
  unsigned int plane_indices[] = {
    0, 1, 3,
    1, 2, 3
  };
  
  GLuint plane_VAO, VBO, EBO;
  glGenVertexArrays(1, &plane_VAO);
  glGenBuffers(1, &VBO);
  glGenBuffers(1, &EBO);
  
  glBindVertexArray(plane_VAO);
  
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(plane_indices), plane_indices, GL_STATIC_DRAW);
  
  glBindBuffer(GL_ARRAY_BUFFER, VBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(plane_vertices), plane_vertices, GL_STATIC_DRAW);
  
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
  
  glBindVertexArray(0);
  
  glDeleteBuffers(1, &VBO);
  glDeleteBuffers(1, &EBO);
  
  mat4 proj = mat4_perspective(45.f, .1f, 1000.f, (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT);
  mat4 view = mat4_view_look_at(vec3_new(0.f, 25.f, -50.f),
                                vec3_new(0.f, -3.f, 0.f),
                                vec3_new(0.f, 1.f, 0.f));
  mat4 board_world = mat4_id();
  
  GLuint board_shader = load_shader_file("res/default.vert.glsl", "res/board.frag.glsl");
  GLuint piece_shader = load_shader_file("res/default.vert.glsl", "res/piece.frag.glsl");
  GLuint font_shader  = load_shader_file("res/font.vert.glsl",    "res/font.frag.glsl");
  
  dict_new(&piece_map);
  
  LOAD_PIECE(board,   0);
  LOAD_PIECE(pawn,   'p');
  LOAD_PIECE(bishop, 'b');
  LOAD_PIECE(knight, 'n');
  LOAD_PIECE(rook,   'r');
  LOAD_PIECE(king,   'k');
  LOAD_PIECE(queen,  'q');
  
  fen_to_grid(current_fen);
  
  thrd_t server;
  thrd_create(&server, server_thread, NULL);
  
  SDL_bool running = SDL_TRUE, dragging = SDL_FALSE;
  SDL_Event e;
  
  Uint32 now = SDL_GetTicks();
  Uint32 then;
  float  delta;
  
  while (running) {
    then = now;
    now = SDL_GetTicks();
    delta = (float)(now - then) / 1000.0f;
    
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT:
          running = SDL_FALSE;
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT)
            dragging = SDL_TRUE;
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT)
            dragging = SDL_FALSE;
          break;
        case SDL_MOUSEMOTION:
          if (dragging && client_connected)
            view = mat4_mul_mat4(view, mat4_rotation_y(DEG2RAD((float)e.motion.xrel * DRAG_SPEED) * delta));
          break;
        case SDL_MOUSEWHEEL:
          break;
      }
    }
    
    if (!client_connected)
      view = mat4_mul_mat4(view, mat4_rotation_y(DEG2RAD(2.f * DRAG_SPEED) * delta));
    
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glUseProgram(board_shader);
    
    glUniformMatrix4fv(glGetUniformLocation(board_shader, "projection"),  1, GL_FALSE, &proj.m[0]);
    glUniformMatrix4fv(glGetUniformLocation(board_shader, "view"),  1, GL_FALSE, &view.m[0]);
    glUniformMatrix4fv(glGetUniformLocation(board_shader, "model"),  1, GL_FALSE, &board_world.m[0]);
    glUniform3f(glGetUniformLocation(board_shader, "viewPos"), view.xw, view.yw, view.zw);
    
    draw_obj(&board);
    
    glUseProgram(piece_shader);
    
    glUniformMatrix4fv(glGetUniformLocation(piece_shader, "projection"),  1, GL_FALSE, &proj.m[0]);
    glUniformMatrix4fv(glGetUniformLocation(piece_shader, "view"),  1, GL_FALSE, &view.m[0]);
    glUniform3f(glGetUniformLocation(piece_shader, "viewPos"), view.xw, view.yw, view.zw);
    
    mat4 top_left = mat4_mul_mat4(mat4_id(), mat4_translation(vec3_new(BOARD_TOP - BOARD_STEP, 0.f, BOARD_TOP)));
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) {
        top_left = mat4_mul_mat4(top_left, mat4_translation(vec3_new(BOARD_STEP, 0.f, 0.f)));
        
        char grid_c = grid[i * 8 + j];
        if (grid_c == 'X')
          continue;
        int is_black = ((int)grid_c < 98);
        
        if (is_black) {
          mat4 tmp = mat4_mul_mat4(top_left, mat4_rotation_y(DEG2RAD(180.f)));
          glUniformMatrix4fv(glGetUniformLocation(piece_shader, "model"),  1, GL_FALSE, &tmp.m[0]);
        } else
          glUniformMatrix4fv(glGetUniformLocation(piece_shader, "model"),  1, GL_FALSE, &top_left.m[0]);
        glUniform1i(glGetUniformLocation(piece_shader, "white"), is_black);
        
        draw_obj(dict_get(&piece_map, grid_c, NULL));
      }
      top_left = mat4_mul_mat4(mat4_id(), mat4_translation(vec3_new(BOARD_TOP - BOARD_STEP, 0.f, BOARD_TOP + ((i + 1) * BOARD_STEP))));
    }
    
    if (!client_connected) {
      glUseProgram(font_shader);
      
      glUniform2f(glGetUniformLocation(font_shader, "iResolution"), SCREEN_WIDTH, SCREEN_HEIGHT);
      glBindVertexArray(plane_VAO);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
    
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    SDL_GL_SwapWindow(window);
  }
  
  free_obj(&board);
  free_obj(&bishop);
  free_obj(&knight);
  free_obj(&rook);
  free_obj(&king);
  free_obj(&queen);
  glDeleteVertexArrays(1, &plane_VAO);
  glDeleteProgram(board_shader);
  glDeleteProgram(piece_shader);
  glDeleteProgram(font_shader);
  dict_destroy(&piece_map);
  
  return 0;
}
